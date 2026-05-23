#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/lib/random.hpp>
#include <nano/lib/rate_limiting.hpp>
#include <nano/lib/thread_pool.hpp>
#include <nano/node/bootstrap/account_sets_index.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/common.hpp>
#include <nano/node/bootstrap/database_scan_index.hpp>
#include <nano/node/bootstrap/frontier_scan_index.hpp>
#include <nano/node/bootstrap/peer_scoring.hpp>
#include <nano/node/bootstrap/throttle.hpp>
#include <nano/node/fwd.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <memory>
#include <thread>
#include <variant>

namespace mi = boost::multi_index;

namespace nano::bootstrap
{
class priority_strategy;
class database_strategy;
class dependency_strategy;
class frontier_strategy;

struct blocks_tag_payload
{
	nano::hash_or_account start{ 0 };
	size_t count{ 0 };
};

struct dependency_tag_payload
{
	nano::block_hash start{ 0 };
};

struct frontier_tag_payload
{
	nano::account start{ 0 };
};

using async_tag_payload = std::variant<blocks_tag_payload, dependency_tag_payload, frontier_tag_payload>;

struct async_tag
{
	using id_t = nano::bootstrap::id_t;

	query_type type{ query_type::invalid };
	query_source source{ query_source::invalid };
	nano::account account{ 0 };
	nano::block_hash hash{ 0 };
	std::chrono::steady_clock::time_point cutoff{};
	std::chrono::steady_clock::time_point timestamp{ std::chrono::steady_clock::now () };
	id_t id{ generate_id () };

	async_tag_payload payload;
};

enum class verify_result
{
	ok,
	nothing_new,
	invalid,
};

class bootstrap_context
{
public:
	bootstrap_context (nano::node_config const &, nano::node &, nano::ledger &, nano::ledger_notifications &, nano::block_processor &, nano::network &, nano::stats &, nano::logger &);
	~bootstrap_context ();

	void start ();
	void stop ();

	void reset ();

	/**
	 * Process bootstrap messages coming from the network
	 */
	void process (nano::messages::asc_pull_ack const & message, std::shared_ptr<nano::transport::channel> const &);

	/* Waits for a condition to be satisfied with incremental backoff */
	void wait (std::function<bool ()> const & predicate) const;

	/* Ensure there is enough space in block_processor for queuing new blocks */
	void wait_block_processor (nano::bootstrap::strategy, std::size_t threshold) const;

	/* Placeholder channel used as a fair-queue partition key so the block processor equalizes ingest across strategies */
	std::shared_ptr<nano::transport::channel> const & block_processor_channel (nano::bootstrap::strategy) const;

	/* Waits for a channel that is not full. Applies the per-strategy rate limiter. */
	std::shared_ptr<nano::transport::channel> wait_channel (nano::bootstrap::strategy strategy);

	bool request (nano::account, size_t count, std::shared_ptr<nano::transport::channel> const &, query_source);
	bool send (std::shared_ptr<nano::transport::channel> const &, nano::messages::asc_pull_req && message, async_tag tag);

	size_t count_tags (nano::account const & account, query_source source) const;
	size_t count_tags (nano::block_hash const & hash, query_source source) const;

	/* Inspects a block that has been processed by the block processor */
	void inspect (secure::transaction const &, nano::block_status const & result, nano::block_context const & context);

	// Calculates a lookback size based on the size of the ledger
	std::size_t compute_throttle_size () const;

	nano::container_info container_info () const;

private:
	bool process (nano::messages::asc_pull_ack::blocks_payload const & response, async_tag const & tag);
	bool process (nano::messages::asc_pull_ack::account_info_payload const & response, async_tag const & tag);
	bool process (nano::messages::asc_pull_ack::frontiers_payload const & response, async_tag const & tag);
	bool process (nano::messages::empty_payload const & response, async_tag const & tag);

	verify_result verify (nano::messages::asc_pull_ack::blocks_payload const & response, async_tag const & tag) const;

	// Filters out blocks already present in the ledger (read transaction) and submits the rest to
	// the block processor. Runs on a bootstrap worker thread, NOT under ctx.mutex, to keep ledger
	// I/O off the message-processing thread that dispatches network responses.
	void submit_blocks (std::deque<std::shared_ptr<nano::block>> blocks, async_tag tag);

	void cleanup ();
	void run_cleanup ();

public: // Dependencies
	nano::bootstrap_config const & config;
	nano::network_constants const & network_constants;
	nano::ledger & ledger;
	nano::ledger_notifications & ledger_notifications;
	nano::block_processor & block_processor;
	nano::network & network;
	nano::stats & stats;
	nano::logger & logger;

public: // Strategies
	std::unique_ptr<nano::bootstrap::priority_strategy> priority_strat_impl;
	nano::bootstrap::priority_strategy & priority_strat;
	std::unique_ptr<nano::bootstrap::database_strategy> database_strat_impl;
	nano::bootstrap::database_strategy & database_strat;
	std::unique_ptr<nano::bootstrap::dependency_strategy> dependency_strat_impl;
	nano::bootstrap::dependency_strategy & dependency_strat;
	std::unique_ptr<nano::bootstrap::frontier_strategy> frontier_strat_impl;
	nano::bootstrap::frontier_strategy & frontier_strat;

public: // Shared state
	nano::bootstrap::account_sets_index accounts;
	nano::bootstrap::database_scan_index database_scan;
	nano::bootstrap::frontier_scan_index frontiers;
	nano::bootstrap::throttle throttle;
	nano::bootstrap::peer_scoring scoring;

	// clang-format off
	class tag_sequenced {};
	class tag_id {};
	class tag_account {};
	class tag_hash {};

	using ordered_tags = boost::multi_index_container<async_tag,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_id>,
			mi::member<async_tag, id_t, &async_tag::id>>,
		mi::hashed_non_unique<mi::tag<tag_account>,
			mi::member<async_tag, nano::account, &async_tag::account>>,
		mi::hashed_non_unique<mi::tag<tag_hash>,
			mi::member<async_tag, nano::block_hash, &async_tag::hash>>
	>>;
	// clang-format on
	ordered_tags tags;

	// Per-strategy rate limiters
	nano::rate_limiter priority_limiter;
	nano::rate_limiter database_limiter;
	nano::rate_limiter dependency_limiter;
	nano::rate_limiter frontier_limiter;

	// Per-strategy placeholder channels. Tagging block_processor submissions with a distinct
	// channel per strategy gives each its own fair-queue bucket, so the processor round-robins
	// ingest evenly across strategies instead of letting one starve the others.
	std::shared_ptr<nano::transport::channel> priority_channel;
	std::shared_ptr<nano::transport::channel> database_channel;
	std::shared_ptr<nano::transport::channel> topology_channel;

	bool stopped{ false };
	mutable nano::mutex mutex;
	mutable nano::condition_variable condition;

	std::thread cleanup_thread;

	nano::thread_pool workers;
	nano::random_generator_mt rng;
};
}
