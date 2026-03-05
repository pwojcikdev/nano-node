#pragma once

#include <nano/lib/interval.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/observer_set.hpp>
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

#include <any>
#include <memory>
#include <thread>

namespace mi = boost::multi_index;

namespace nano::bootstrap
{
class priority_strategy;
class database_strategy;
class dependency_strategy;
class frontier_strategy;
}

namespace nano
{
class bootstrap_service
{
	friend class nano::bootstrap::priority_strategy;
	friend class nano::bootstrap::database_strategy;
	friend class nano::bootstrap::dependency_strategy;
	friend class nano::bootstrap::frontier_strategy;

public:
	bootstrap_service (nano::node_config const &, nano::ledger &, nano::ledger_notifications &, nano::block_processor &, nano::network &, nano::stats &, nano::logger &);
	~bootstrap_service ();

	void start ();
	void stop ();

	/**
	 * Process bootstrap messages coming from the network
	 */
	void process (nano::messages::asc_pull_ack const & message, std::shared_ptr<nano::transport::channel> const &);

	/**
	 * Clears priority and blocking accounts state
	 */
	void reset ();

	/**
	 * Adds an account to the priority set
	 */
	void prioritize (nano::account const & account);

	std::size_t blocked_size () const;
	std::size_t priority_size () const;
	std::size_t score_size () const;

	bool prioritized (nano::account const &) const;
	bool blocked (nano::account const &) const;

	nano::bootstrap::account_sets_index::info_t info () const;

	struct status_result
	{
		size_t priorities;
		size_t blocking;
	};
	status_result status () const;

	nano::container_info container_info () const;

private: // Dependencies
	bootstrap_config const & config;
	nano::network_constants const & network_constants;
	nano::ledger & ledger;
	nano::ledger_notifications & ledger_notifications;
	nano::block_processor & block_processor;
	nano::network & network;
	nano::stats & stats;
	nano::logger & logger;

public: // Tag
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

	struct async_tag
	{
		using id_t = nano::bootstrap::id_t;

		nano::bootstrap::query_type type{ nano::bootstrap::query_type::invalid };
		nano::bootstrap::query_source source{ nano::bootstrap::query_source::invalid };
		nano::account account{ 0 };
		nano::block_hash hash{ 0 };
		std::chrono::steady_clock::time_point cutoff{};
		std::chrono::steady_clock::time_point timestamp{ std::chrono::steady_clock::now () };
		id_t id{ nano::bootstrap::generate_id () };

		std::any payload; // Strategy-specific data (blocks_tag_payload, dependency_tag_payload, frontier_tag_payload)
	};

private:
	/* Inspects a block that has been processed by the block processor */
	void inspect (secure::transaction const &, nano::block_status const & result, nano::block const & block, nano::block_source);

	void run_timeouts ();
	void cleanup_and_sync ();

	/* Waits for a condition to be satisfied with incremental backoff */
	void wait (std::function<bool ()> const & predicate) const;

	/* Ensure there is enough space in block_processor for queuing new blocks */
	void wait_block_processor () const;
	/* Waits for a channel that is not full */
	std::shared_ptr<nano::transport::channel> wait_channel ();
	bool request (nano::account, size_t count, std::shared_ptr<nano::transport::channel> const &, nano::bootstrap::query_source);
	bool send (std::shared_ptr<nano::transport::channel> const &, nano::messages::asc_pull_req message, async_tag tag);

	bool process (nano::messages::asc_pull_ack::blocks_payload const & response, async_tag const & tag);
	bool process (nano::messages::asc_pull_ack::account_info_payload const & response, async_tag const & tag);
	bool process (nano::messages::asc_pull_ack::frontiers_payload const & response, async_tag const & tag);
	bool process (nano::messages::empty_payload const & response, async_tag const & tag);

public:
	enum class verify_result
	{
		ok,
		nothing_new,
		invalid,
	};

private:
	/**
	 * Verifies whether the received response is valid. Returns:
	 * - invalid: when received blocks do not correspond to requested hash/account or they do not make a valid chain
	 * - nothing_new: when received response indicates that the account chain does not have more blocks
	 * - ok: otherwise, if all checks pass
	 */
	verify_result verify (nano::messages::asc_pull_ack::blocks_payload const & response, async_tag const & tag) const;

	size_t count_tags (nano::account const & account, nano::bootstrap::query_source source) const;
	size_t count_tags (nano::block_hash const & hash, nano::bootstrap::query_source source) const;

	// Calculates a lookback size based on the size of the ledger where larger ledgers have a larger sample count
	std::size_t compute_throttle_size () const;

private:
	std::unique_ptr<nano::bootstrap::priority_strategy> priority_strat_impl;
	nano::bootstrap::priority_strategy & priority_strat;
	std::unique_ptr<nano::bootstrap::database_strategy> database_strat_impl;
	nano::bootstrap::database_strategy & database_strat;
	std::unique_ptr<nano::bootstrap::dependency_strategy> dependency_strat_impl;
	nano::bootstrap::dependency_strategy & dependency_strat;
	std::unique_ptr<nano::bootstrap::frontier_strategy> frontier_strat_impl;
	nano::bootstrap::frontier_strategy & frontier_strat;

	nano::bootstrap::account_sets_index accounts;
	nano::bootstrap::database_scan_index database_scan;
	nano::bootstrap::throttle throttle;
	nano::bootstrap::peer_scoring scoring;
	nano::bootstrap::frontier_scan_index frontiers;

	// clang-format off
	class tag_sequenced {};
	class tag_id {};
	class tag_account {};
	class tag_hash {};

	using ordered_tags = boost::multi_index_container<async_tag,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_id>,
			mi::member<async_tag, nano::bootstrap::id_t, &async_tag::id>>,
		mi::hashed_non_unique<mi::tag<tag_account>,
			mi::member<async_tag, nano::account , &async_tag::account>>,
		mi::hashed_non_unique<mi::tag<tag_hash>,
			mi::member<async_tag, nano::block_hash, &async_tag::hash>>
	>>;
	// clang-format on
	ordered_tags tags;

	// Rate limiter for all types of requests
	nano::rate_limiter limiter;
	// Requests for accounts from database have much lower hitrate and could introduce strain on the network
	// A separate (lower) limiter ensures that we always reserve resources for querying accounts from priority queue
	nano::rate_limiter database_limiter;
	// Rate limiter for frontier requests
	nano::rate_limiter frontiers_limiter;

	nano::interval sync_dependencies_interval;

	bool stopped{ false };
	mutable nano::mutex mutex;
	mutable nano::condition_variable condition;
	std::thread cleanup_thread;

	nano::thread_pool workers;
	nano::random_generator_mt rng;
};
}
