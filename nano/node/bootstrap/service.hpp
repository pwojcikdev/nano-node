#pragma once

#include <nano/lib/async.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/rate_limiting.hpp>
#include <nano/lib/thread_pool.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/bootstrap/account_sets_index.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/common.hpp>
#include <nano/node/bootstrap/database_scan_index.hpp>
#include <nano/node/bootstrap/peer_pool.hpp>
#include <nano/node/bootstrap/queries.hpp>
#include <nano/node/bootstrap/throttle.hpp>
#include <nano/node/fwd.hpp>
#include <nano/secure/fwd.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <random>
#include <unordered_map>

namespace nano::bootstrap
{
// A type-checked response delivered to the request that asked for it
struct reply
{
	nano::messages::asc_pull_ack message;
	std::shared_ptr<nano::transport::channel> channel;
};

// The conclusion of a single request, delivered as a value to the awaiting strategy coroutine
struct request_outcome
{
	enum class status_t
	{
		response, // A type-checked response arrived
		timeout, // The peer did not respond in time
		send_failed, // The request never made it to the wire
	};

	status_t status{ status_t::send_failed };
	std::optional<nano::messages::asc_pull_ack> message;
	std::shared_ptr<nano::transport::channel> channel; // The channel the response arrived on

	explicit operator bool () const
	{
		return status == status_t::response;
	}
};

/*
 * The coroutine-driven bootstrap engine. One strand confines all shared state (indexes, peers,
 * in-flight bookkeeping); strategies run as cooperating coroutines on it, blocking ledger reads are
 * offloaded to a small worker pool, and the entire request lifecycle lives in the request () primitive.
 * The executor, clock and peer source are injected so tests can drive the subsystem deterministically.
 */
class service
{
public:
	// Periodically polled source of bootstrap-capable peers; may block (it is called off the strand)
	using peer_source_t = std::function<std::deque<std::shared_ptr<nano::transport::channel>> ()>;

	service (nano::node_config const &, nano::ledger &, nano::ledger_notifications &, nano::block_processor &, peer_source_t, nano::stats &, nano::logger &,
	asio::io_context &, nano::async::clock &, unsigned rng_seed);
	~service ();

	void start ();
	// Cancels and joins all coroutines; must not be called from an io thread
	void stop ();
	// Requests a restart: in-flight work is abandoned and all indexes are cleared. Non-blocking.
	void reset ();

	// Process bootstrap messages coming from the network, safe to call from any thread
	void process (nano::messages::asc_pull_ack const & message, std::shared_ptr<nano::transport::channel> const &);

	nano::container_info container_info () const;

public: // Accessors for RPC and tests, dispatched onto the strand
	void prioritize (nano::account const &);
	std::size_t priority_size () const;
	std::size_t blocked_size () const;
	bool prioritized (nano::account const &) const;
	bool blocked (nano::account const &) const;
	account_sets_index::info_t info () const;

public: // Dependencies
	nano::bootstrap_config const & config;
	nano::network_constants const & network_constants;
	nano::ledger & ledger;
	nano::ledger_notifications & ledger_notifications;
	nano::block_processor & block_processor;
	peer_source_t peer_source;
	nano::stats & stats;
	nano::logger & logger;

public: // Execution context
	nano::async::strand strand;
	nano::async::clock & clock;
	nano::thread_pool workers; // Blocking ledger reads run here, never on the strand

public: // Strand-confined state
	account_sets_index accounts;
	database_scan_index database_scan;
	nano::bootstrap::throttle throttle;
	peer_pool peers;

	// Wake-up topology: notified whenever the respective state changes
	nano::async::condition_any accounts_changed;
	nano::async::condition_any peers_changed;
	nano::async::condition_any processor_drained;

	// Global backpressure: bounds the number of in-flight requests (the old `tags.size () < max_requests`)
	nano::async::semaphore request_budget;

	// Rate limiters, shared across strategies as today
	nano::rate_limiter limiter;
	nano::rate_limiter database_limiter;
	nano::rate_limiter frontiers_limiter;

public: // Coroutine toolkit used by the strategies (strand only)
	/*
	 * Owns the entire lifecycle of one request: correlation, wire-send confirmation with a generous
	 * grace period (a dying channel may silently drop its send callback), and the response timeout.
	 * A failed send or timeout deliberately leaves the peer slot reserved as an implicit penalty;
	 * only the caller releases it, on a processed response. The budget token is held for the
	 * request's whole lifetime and returns to the pool when the outcome is known.
	 */
	asio::awaitable<request_outcome> request (std::shared_ptr<nano::transport::channel>, query_descriptor query, query_source, nano::async::semaphore::token budget);

	// Waits until the rate limiter passes the given cost
	asio::awaitable<void> wait_limiter (nano::rate_limiter &, std::size_t cost);

	// Waits until the block processor has capacity for more bootstrap blocks
	asio::awaitable<void> wait_block_processor ();

	// Waits until any peer can be reserved (no exclusion); fanout rounds do their own keyed acquisition
	asio::awaitable<std::shared_ptr<nano::transport::channel>> wait_channel ();

	// Verifies, submits to the block processor and accounts the outcome of a blocks response.
	// Shared by the priority and database strategies, which only differ in their pacing and sources.
	// Returns the request status so the caller can react to failed sends and timeouts.
	asio::awaitable<request_outcome::status_t> run_pull (std::shared_ptr<nano::transport::channel>, blocks_query, query_source, nano::async::semaphore::token budget);

	// Pseudo random number in [0, max), seeded at construction for reproducible tests
	unsigned random (unsigned max);

private:
	asio::awaitable<void> run_supervisor ();
	asio::awaitable<void> run_maintenance ();

	// Delivers a response to the request awaiting it, on the strand
	void deliver (nano::messages::asc_pull_ack const & message, std::shared_ptr<nano::transport::channel> const &);

	// Clears all bootstrap state, used by reset () between strategy generations
	void reset_state ();

	std::size_t compute_throttle_size () const;

	// Ledger-dependent inspection values are resolved on the notification thread inside its read
	// transaction; the decisions against live accounts state are applied on the strand
	struct inspect_command
	{
		enum class kind_t
		{
			progress,
			gap_source,
			gap_previous,
			gap_epoch_open,
		};

		kind_t kind;
		nano::account account{ 0 };
		nano::block_hash hash{ 0 };
		nano::account destination{ 0 };
		bool is_send{ false };
	};

	static std::optional<inspect_command> resolve_inspect (nano::ledger &, secure::transaction const &, nano::block_status const &, nano::block const &, nano::block_source);
	void apply_inspect (inspect_command const &);

private:
	struct pending_request
	{
		query_type type;
		query_source source;
		std::chrono::steady_clock::time_point timestamp;
		std::shared_ptr<nano::async::oneshot<reply>> slot;
	};

	// Response correlation by id; all other jobs of the old tags multi index live elsewhere:
	// dedup in strategy-local registries, backpressure in the request budget semaphore, and
	// timeouts inside each request () frame
	std::unordered_map<id_t, pending_request> requests;

	// Strategies
	std::unique_ptr<frontier_strategy> frontier;
	std::unique_ptr<priority_strategy> priority;
	std::unique_ptr<database_strategy> database;
	std::unique_ptr<dependency_strategy> dependency;

	// Supervisor lifecycle
	std::optional<nano::async::task> supervisor;
	nano::async::condition_any restart_condition;
	bool restart_requested{ false }; // Strand-confined
	std::atomic<bool> stopped{ false };

	// Bounds the response ingress so that a flood of unsolicited replies cannot grow the post queue
	// without limit; today's inline processing under the mutex provided this backpressure implicitly
	std::atomic<std::size_t> pending_deliveries{ 0 };

	std::default_random_engine rng;

public:
	// Helper for accessors: runs the function on the strand and waits for the result
	template <typename F>
	auto on_strand (F && function) const
	{
		debug_assert (!strand.running_in_this_thread ());
		return asio::dispatch (strand, asio::use_future (std::forward<F> (function))).get ();
	}
};
}
