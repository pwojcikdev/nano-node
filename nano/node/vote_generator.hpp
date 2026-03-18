#pragma once

#include <nano/lib/interval.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/processing_queue.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/voting/vote_broadcaster.hpp>
#include <nano/node/voting/vote_factory.hpp>
#include <nano/secure/common.hpp>

#include <condition_variable>
#include <deque>
#include <thread>

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
 * Generates and broadcasts non-final (normal) votes.
 * Uses vote_factory for checking and signing, vote_broadcaster for delivery.
 */
class vote_generator final
{
public:
	vote_generator (vote_generator_config const &, nano::vote_factory &, nano::ledger &, nano::vote_processor &, nano::local_vote_history &, nano::network &, nano::network_params &, nano::stats &, nano::logger &, std::shared_ptr<nano::transport::channel> inproc_channel);
	~vote_generator ();

	/** Queue items for normal vote generation, categorized by bucket */
	void add (nano::root const &, nano::block_hash const &, nano::bucket_index bucket = 0);

	void start ();
	void stop ();

	nano::container_info container_info () const;

private:
	using queue_entry_t = std::pair<nano::root, nano::block_hash>;

	void run ();
	void broadcast (nano::unique_lock<nano::mutex> &);
	void process_batch (std::deque<queue_entry_t> & batch);
	bool broadcast_predicate () const;

private: // Dependencies
	vote_generator_config const & config;
	nano::vote_factory & factory;
	nano::ledger & ledger;
	nano::stats & stats;
	nano::logger & logger;
	std::unique_ptr<nano::vote_spacing> spacing_impl;
	nano::vote_spacing & spacing;
	nano::vote_broadcaster broadcaster;

private:
	mutable nano::mutex mutex;
	nano::condition_variable condition;
	std::deque<nano::vote_factory::verified_candidate> candidates;
	std::atomic<bool> stopped{ false };
	std::thread thread;
	nano::processing_queue<queue_entry_t> vote_generation_queue;
	std::chrono::steady_clock::time_point next_broadcast{ std::chrono::steady_clock::now () };

	nano::interval log_interval;
};
}
