#pragma once

#include <nano/lib/container_info.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/work_generation.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>

namespace boost::asio
{
class io_context;
}

namespace nano
{
/**
 * Generates proof of work by fanning out requests to the configured work peers, racing the local work pool when available.
 * The first valid result wins and the remaining requests are cancelled.
 * When all peers fail and the local work pool cannot decide the outcome, the request is retried with exponential backoff.
 */
class work_generator final
{
public:
	work_generator (nano::node_config const &, nano::network_params const &, nano::work_pool &, nano::thread_pool & workers, nano::node_observers &, nano::stats &, nano::logger &, boost::asio::io_context &);
	~work_generator ();

	void stop ();

	// Asynchronously generates work for the request, the callback fires exactly once
	void generate (nano::work_request, nano::work_callback);
	// Convenience overload targeting the primary work peers from the node config
	void generate (nano::work_version, nano::root const &, uint64_t difficulty, nano::work_callback, std::optional<nano::account> const & = std::nullopt);

	// Cancels all in-flight generations for the given root
	void cancel (nano::root const &);

	// Local work pool can generate work
	bool local_enabled () const;
	// Local work pool or the primary work peers from the node config can generate work
	bool enabled () const;
	// Local work pool or the given peers can generate work
	bool enabled (std::vector<nano::work_peer> const &) const;

	// Number of in-flight generations
	size_t size () const;

	nano::container_info container_info () const;

public: // Dependencies
	nano::node_config const & config;
	nano::network_params const & network_params;
	nano::work_pool & work_pool;
	nano::thread_pool & workers;
	nano::node_observers & observers;
	nano::stats & stats;
	nano::logger & logger;
	boost::asio::io_context & io_ctx;

private:
	class generation;

	// In-flight generation with its completion callback and the backoff to apply if it has to be retried
	struct entry
	{
		std::shared_ptr<generation> gen;
		nano::work_callback callback;
		std::chrono::seconds backoff;
	};

	// Creates and starts a generation, invokes the callback with std::nullopt if the request cannot be serviced
	void submit (nano::work_request const &, nano::work_callback const &, std::chrono::seconds backoff);
	// Removes the finished generation and dispatches its outcome, `retry` requests another attempt instead of failing
	void finished (generation const &, nano::work_generation_result, bool retry);
	// Schedules another attempt after all peers failed to generate
	void retry (nano::work_request const &, nano::work_callback const &, std::chrono::seconds backoff);

	static std::chrono::seconds constexpr initial_backoff{ 1 };
	static std::chrono::seconds constexpr max_backoff{ 5 * 60 };

private:
	std::unordered_multimap<nano::root, entry> generations;

	// Set when all peers failed to generate, cleared on a valid peer response; makes the local work pool join the race right away
	std::atomic<bool> unresponsive_peers{ false };
	std::atomic<bool> stopped{ false };
	mutable nano::mutex mutex;
};
}
