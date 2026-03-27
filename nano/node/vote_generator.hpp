#pragma once

#include <nano/lib/interval.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/lib/processing_queue.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/fair_queue.hpp>
#include <nano/node/fwd.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/voting_policy.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>

namespace mi = boost::multi_index;

namespace nano
{
class vote_generator_config final
{
public:
	nano::error serialize (nano::tomlconfig & toml) const;
	nano::error deserialize (nano::tomlconfig & toml);

public:
	size_t max_queue{ 1024 * 32 };
	size_t batch_size{ 256 };
	std::chrono::milliseconds delay{ 100ms };
};

/**
 * Fair queue over balance buckets with deduplication by root
 * Replaces existing entry when a new hash arrives for the same root
 * Holds ca
 */
class vote_generator_index final
{
public:
	using entry = std::pair<nano::root, nano::block_hash>;

	explicit vote_generator_index (size_t max_size_per_bucket);

	/// Push a request. Returns true if added or replaced, false if duplicate (same root+hash) or queue full
	bool push (nano::bucket_index bucket, nano::root const & root, nano::block_hash const & hash);

	/// Remove and return up to `count` valid entries (stale entries from replacements are skipped)
	std::deque<entry> next_batch (size_t count);

	size_t size () const;
	bool empty () const;

private:
	nano::fair_queue<entry, nano::bucket_index> queue;
	std::unordered_map<nano::root, nano::block_hash> dedup;
};

/**
 * Holds vote permits in FIFO order with deduplication by root
 * Provides batch extraction from the front
 */
class vote_broadcast_index final
{
public:
	explicit vote_broadcast_index (size_t max_size);

	/// Insert a permit keyed by root. If a permit for the same root already exists, the new one is dropped
	bool push (nano::qualified_root const & root, nano::vote_permit permit);

	/// Remove the entry for the given root. Returns true if erased
	bool erase (nano::qualified_root const & root);

	/// Remove and return up to `count` entries from the front (FIFO order)
	std::deque<nano::vote_permit> next_batch (size_t count);

	size_t size () const;
	bool empty () const;

private:
	size_t const max_size;

	struct entry
	{
		nano::qualified_root root;
		nano::vote_permit permit;
	};

	// clang-format off
	class tag_sequenced {};
	class tag_root {};

	using ordered_entries = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<entry, nano::qualified_root, &entry::root>>
	>>;
	// clang-format on

	ordered_entries entries;
};

class vote_generator final
{
private:
	using queue_entry_t = std::pair<nano::root, nano::block_hash>;
	std::chrono::steady_clock::time_point next_broadcast = { std::chrono::steady_clock::now () };

public:
	vote_generator (vote_generator_config const &, nano::node &, nano::voting_policy &, nano::ledger &, nano::wallets &, nano::vote_processor &, nano::local_vote_history &, nano::network &, nano::stats &, nano::logger &, bool is_final, std::shared_ptr<nano::transport::channel> inproc_channel);
	~vote_generator ();

	/** Queue items for vote generation, or broadcast votes already in cache */
	void add (nano::root const &, nano::block_hash const &);

	void start ();
	void stop ();

	nano::container_info container_info () const;

private:
	void run ();
	void broadcast (nano::unique_lock<nano::mutex> &);
	void broadcast_action (std::shared_ptr<nano::vote> const &) const;
	void process_batch (std::deque<queue_entry_t> & batch);
	bool broadcast_predicate () const;

	nano::stat::type stat_type () const;
	nano::log::type log_type () const;

private: // Dependencies
	vote_generator_config const & config;
	nano::node & node;
	nano::voting_policy & policy;
	nano::ledger & ledger;
	nano::wallets & wallets;
	nano::vote_processor & vote_processor;
	nano::local_vote_history & history;
	std::unique_ptr<nano::vote_spacing> spacing_impl;
	nano::vote_spacing & spacing;
	nano::network & network;
	nano::stats & stats;
	nano::logger & logger;

private:
	const bool is_final;
	mutable nano::mutex mutex;
	nano::condition_variable condition;
	std::deque<nano::vote_permit> candidates;
	std::atomic<bool> stopped{ false };
	std::thread thread;
	std::shared_ptr<nano::transport::channel> inproc_channel;
	nano::processing_queue<queue_entry_t> vote_generation_queue;

	nano::interval log_interval;
};
}
