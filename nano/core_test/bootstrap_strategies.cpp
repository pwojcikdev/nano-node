#include <nano/lib/blocks.hpp>
#include <nano/lib/stats.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/block_processor.hpp>
#include <nano/node/bootstrap/service.hpp>
#include <nano/node/ledger_notifications.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/test_channel.hpp>
#include <nano/node/unchecked_map.hpp>
#include <nano/test_common/async.hpp>
#include <nano/test_common/ledger_context.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

/*
 * Deterministic tests of the coroutine bootstrap engine: the test owns the io_context and steps it
 * manually, timeouts and cooldowns are driven by virtual time, and peers are scripted test channels
 * whose send callbacks fire inline. Only the worker pool (ledger reads) runs on a real thread, which
 * the poll helpers accommodate with bounded real-time polling.
 */

namespace
{
class bootstrap_fixture
{
public:
	explicit bootstrap_fixture (std::function<void (nano::node_config &)> configure = {}) :
		ledger_ctx{ nano::test::ledger_empty () }
	{
		// Neutralize the rate limiters (0 = unbounded) so tests control pacing through virtual time only
		config.bootstrap->rate_limit = 0;
		config.bootstrap->database_rate_limit = 0;
		config.bootstrap->frontier_rate_limit = 0;

		// Only the strategy under test should run; tests opt back in as needed
		config.bootstrap->enable_priorities = false;
		config.bootstrap->enable_database_scan = false;
		config.bootstrap->enable_dependency_walker = false;
		config.bootstrap->enable_frontier_scan = false;

		if (configure)
		{
			configure (config);
		}

		service = std::make_unique<nano::bootstrap::service> (
		config, ledger_ctx.ledger (), notifications, processor,
		[this] () {
			nano::lock_guard<nano::mutex> lock{ mutex };
			return channels;
		},
		stats (), logger (), io, clock, /* rng seed */ 42);
	}

	~bootstrap_fixture ()
	{
		shutdown ();
	}

	nano::stats & stats ()
	{
		return ledger_ctx.stats ();
	}

	nano::logger & logger ()
	{
		return ledger_ctx.logger ();
	}

	// Adds a scripted peer; every request it receives is recorded together with the channel index
	std::shared_ptr<nano::transport::test_channel> add_peer ()
	{
		auto channel = std::make_shared<nano::transport::test_channel> (stats (), config.network_params.network.protocol_version);
		channel->set_node_id (nano::account{ next_node_id++ });
		channel->observe<nano::messages::asc_pull_req> ([this, index = peers.size ()] (nano::messages::asc_pull_req const & request) {
			nano::lock_guard<nano::mutex> lock{ mutex };
			requests.push_back ({ index, request });
		});
		{
			nano::lock_guard<nano::mutex> lock{ mutex };
			channels.push_back (channel);
		}
		peers.push_back (channel);
		return channel;
	}

	void start ()
	{
		service->start ();
		flush ();
	}

	// Runs everything that is ready on the manually stepped io_context
	void flush ()
	{
		io.restart ();
		io.poll ();
	}

	// Advances virtual time and runs everything that became ready
	void advance (std::chrono::steady_clock::duration duration)
	{
		clock.advance (duration);
		flush ();
	}

	// Polls until the predicate holds; the bounded real-time loop accommodates worker pool results
	bool poll_until (std::function<bool ()> predicate, std::chrono::milliseconds timeout = 5s)
	{
		auto const deadline = std::chrono::steady_clock::now () + timeout;
		while (!predicate ())
		{
			if (std::chrono::steady_clock::now () > deadline)
			{
				return false;
			}
			flush ();
			std::this_thread::sleep_for (1ms);
		}
		return true;
	}

	// Runs the function on the strand and returns its result; only usable between polls
	template <typename F>
	auto query (F function)
	{
		std::optional<decltype (function ())> result;
		asio::post (service->strand, [&] () {
			result = function ();
		});
		flush ();
		release_assert (result.has_value ());
		return *result;
	}

	struct recorded_request
	{
		std::size_t peer;
		nano::messages::asc_pull_req request;
	};

	std::size_t request_count ()
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return requests.size ();
	}

	recorded_request request (std::size_t index)
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		release_assert (index < requests.size ());
		return requests[index];
	}

	// Delivers a frontiers response for the given recorded request
	void respond_frontiers (recorded_request const & to, std::deque<std::pair<nano::account, nano::block_hash>> frontiers)
	{
		nano::messages::asc_pull_ack response{ config.network_params.network };
		response.id = to.request.id;
		response.type = nano::messages::asc_pull_type::frontiers;
		nano::messages::asc_pull_ack::frontiers_payload payload{};
		payload.frontiers = std::move (frontiers);
		response.payload = payload;
		response.update_header ();
		service->process (response, peers.at (to.peer));
		flush ();
	}

	// Delivers a blocks response for the given recorded request
	void respond_blocks (recorded_request const & to, std::deque<std::shared_ptr<nano::block>> blocks)
	{
		nano::messages::asc_pull_ack response{ config.network_params.network };
		response.id = to.request.id;
		response.type = nano::messages::asc_pull_type::blocks;
		nano::messages::asc_pull_ack::blocks_payload payload{};
		payload.blocks = std::move (blocks);
		response.payload = payload;
		response.update_header ();
		service->process (response, peers.at (to.peer));
		flush ();
	}

	// service::stop () joins coroutines whose handlers run on this io_context, so it must be
	// driven from another thread while this one keeps stepping the executor
	void shutdown ()
	{
		if (stopped)
		{
			return;
		}
		stopped = true;

		std::atomic<bool> done{ false };
		std::thread stopper{ [this, &done] () {
			service->stop ();
			done = true;
		} };
		while (!done)
		{
			flush ();
			std::this_thread::sleep_for (1ms);
		}
		stopper.join ();
	}

public:
	nano::test::ledger_context ledger_ctx;
	nano::node_config config;

	nano::ledger_notifications notifications{ config, ledger_ctx.stats (), ledger_ctx.logger () };
	bool processor_delete{ false };
	nano::unchecked_map unchecked{ 65536, ledger_ctx.stats (), processor_delete };
	nano::block_processor processor{ config, ledger_ctx.ledger (), notifications, unchecked, ledger_ctx.stats (), ledger_ctx.logger () };

	asio::io_context io;
	nano::test::manual_clock clock;

	std::unique_ptr<nano::bootstrap::service> service;

	std::vector<std::shared_ptr<nano::transport::test_channel>> peers;

private:
	mutable nano::mutex mutex;
	std::deque<std::shared_ptr<nano::transport::channel>> channels;
	std::deque<recorded_request> requests;
	uint64_t next_node_id{ 1000 };
	bool stopped{ false };
};

nano::account frontier_start (nano::messages::asc_pull_req const & request)
{
	return std::get<nano::messages::asc_pull_req::frontiers_payload> (request.payload).start;
}

std::function<void (nano::node_config &)> frontier_config (unsigned consideration_count, std::size_t channel_limit = 16)
{
	return [=] (nano::node_config & config) {
		config.bootstrap->enable_frontier_scan = true;
		config.bootstrap->frontier_scan.head_parallelism = 1;
		config.bootstrap->frontier_scan.consideration_count = consideration_count;
		config.bootstrap->channel_limit = channel_limit;
	};
}
}

TEST (bootstrap_strategies, start_stop)
{
	bootstrap_fixture fixture;
	fixture.start ();
	fixture.flush ();
	// Teardown through the fixture destructor must join all coroutines cleanly
}

// One fanout round: the quorum of samples goes to distinct peers and the head advances to the
// largest gathered candidate, observable through the next round's request start
TEST (bootstrap_strategies, frontier_round_distinct_peers)
{
	bootstrap_fixture fixture{ frontier_config (/* consideration_count */ 2) };
	fixture.add_peer ();
	fixture.add_peer ();
	fixture.add_peer ();
	fixture.start ();

	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= 2; }));

	auto first = fixture.request (0);
	auto second = fixture.request (1);
	ASSERT_NE (first.peer, second.peer); // The distinct-peer fanout rule
	ASSERT_EQ (frontier_start (first.request).number (), 1); // Both samples ask for the same position
	ASSERT_EQ (frontier_start (second.request).number (), 1);

	fixture.respond_frontiers (first, { { nano::account{ 5 }, nano::block_hash{ 5 } } });
	fixture.respond_frontiers (second, { { nano::account{ 7 }, nano::block_hash{ 7 } } });

	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.stats ().count (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::done) >= 1; }));

	// The next round resumes from the largest candidate
	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= 3; }));
	ASSERT_EQ (frontier_start (fixture.request (2).request), nano::account{ 7 });
}

// When every distinct peer was sampled and responses are in, the round concludes early with what
// was gathered instead of waiting for the full quorum
TEST (bootstrap_strategies, frontier_round_concludes_on_peer_exhaustion)
{
	bootstrap_fixture fixture{ frontier_config (/* consideration_count */ 4) };
	fixture.add_peer ();
	fixture.add_peer ();
	fixture.start ();

	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= 2; }));

	fixture.respond_frontiers (fixture.request (0), { { nano::account{ 5 }, nano::block_hash{ 5 } } });
	fixture.respond_frontiers (fixture.request (1), { { nano::account{ 9 }, nano::block_hash{ 9 } } });

	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.stats ().count (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::done_partial) >= 1; }));

	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= 3; }));
	ASSERT_EQ (frontier_start (fixture.request (2).request), nano::account{ 9 });
}

// A timed out request leaves the peer capacity slot reserved as an implicit penalty; the periodic
// pool decay later heals it. The timeout itself fires on virtual time, no real waiting.
TEST (bootstrap_strategies, frontier_timeout_keeps_peer_reserved)
{
	bootstrap_fixture fixture{ [] (nano::node_config & config) {
		frontier_config (/* consideration_count */ 1, /* channel_limit */ 1) (config);
		// Shorter than the maintenance interval, so the decay cannot heal the slot before the assertions
		config.bootstrap->request_timeout = 100ms;
	} };
	fixture.add_peer ();
	fixture.start ();

	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= 1; }));
	ASSERT_EQ (fixture.query ([&] () { return fixture.service->peers.available (); }), 0); // The only slot is in use

	fixture.advance (fixture.config.bootstrap->request_timeout); // The response window expires

	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.stats ().count (nano::stat::type::bootstrap, nano::stat::detail::timeout) >= 1; }));
	ASSERT_EQ (fixture.query ([&] () { return fixture.service->peers.available (); }), 0); // No release: implicit penalty

	// The maintenance decay eventually returns the capacity
	ASSERT_TRUE (fixture.poll_until ([&] () {
		fixture.advance (500ms);
		return fixture.query ([&] () { return fixture.service->peers.available (); }) > 0;
	}));
}

// Unanimous empty samples conclude that the range holds nothing and the scan wraps around
TEST (bootstrap_strategies, frontier_empty_range_wraps)
{
	bootstrap_fixture fixture{ frontier_config (/* consideration_count */ 1) };
	fixture.add_peer ();
	fixture.add_peer ();
	fixture.start ();

	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= 1; }));
	fixture.respond_frontiers (fixture.request (0), {});

	// One empty sample is not enough to give up; after the cooldown a second distinct peer is sampled
	fixture.advance (fixture.config.bootstrap->frontier_scan.cooldown);
	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= 2; }));
	ASSERT_NE (fixture.request (0).peer, fixture.request (1).peer);
	fixture.respond_frontiers (fixture.request (1), {});

	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.stats ().count (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::done_empty) >= 1; }));
	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.stats ().count (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::done_range) >= 1; }));

	// The scan wrapped and resumes from the range start
	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= 3; }));
	ASSERT_EQ (frontier_start (fixture.request (2).request).number (), 1);
}

// A straggler response from an already settled round still releases the peer capacity but feeds nothing
TEST (bootstrap_strategies, frontier_straggler_releases_capacity)
{
	bootstrap_fixture fixture{ frontier_config (/* consideration_count */ 1, /* channel_limit */ 1) };
	fixture.add_peer ();
	fixture.add_peer ();
	fixture.start ();

	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= 1; }));

	// The re-sample of the stuck round goes out after the cooldown, to the other peer
	fixture.advance (fixture.config.bootstrap->frontier_scan.cooldown);
	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= 2; }));
	ASSERT_NE (fixture.request (0).peer, fixture.request (1).peer);

	// The first response settles the round while the second request is still in flight
	fixture.respond_frontiers (fixture.request (0), { { nano::account{ 5 }, nano::block_hash{ 5 } } });
	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.stats ().count (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::done) >= 1; }));

	// The straggler arrives for the settled round: capacity returns, the settled round stays settled
	fixture.respond_frontiers (fixture.request (1), { { nano::account{ 9 }, nano::block_hash{ 9 } } });
	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.query ([&] () { return fixture.service->peers.available (); }) >= 1; }));
	ASSERT_EQ (fixture.stats ().count (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::done), 1);
}

// reset () abandons in-flight work, clears the indexes and restarts the strategies
TEST (bootstrap_strategies, reset_restarts)
{
	bootstrap_fixture fixture{ frontier_config (/* consideration_count */ 2) };
	fixture.add_peer ();
	fixture.add_peer ();
	fixture.start ();

	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= 2; }));

	fixture.service->reset ();
	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.stats ().count (nano::stat::type::bootstrap, nano::stat::detail::reset) >= 1; }));

	// A fresh strategy generation starts scanning from the range start again
	auto const before = fixture.request_count ();
	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= before + 1; }));
	ASSERT_EQ (frontier_start (fixture.request (before).request).number (), 1);
}

// The priority strategy pulls blocks for prioritized accounts; an empty response demotes the account
TEST (bootstrap_strategies, priority_pull_nothing_new)
{
	bootstrap_fixture fixture{ [] (nano::node_config & config) {
		config.bootstrap->enable_priorities = true;
	} };
	fixture.add_peer ();
	fixture.start ();

	nano::keypair key;
	asio::post (fixture.service->strand, [&] () {
		fixture.service->accounts.priority_set (key.pub);
		fixture.service->accounts_changed.notify_all ();
	});

	// The genesis account is prioritized on startup, so requests may cover both accounts
	ASSERT_TRUE (fixture.poll_until ([&] () { return fixture.request_count () >= 1; }));

	// Answer every request with an empty blocks response
	std::size_t answered = 0;
	ASSERT_TRUE (fixture.poll_until ([&] () {
		while (answered < fixture.request_count ())
		{
			fixture.respond_blocks (fixture.request (answered), {});
			++answered;
		}
		return fixture.stats ().count (nano::stat::type::bootstrap_verify_blocks, nano::stat::detail::nothing_new) >= 1;
	}));

	// The empty response demoted the account and reset its cooldown
	ASSERT_GE (fixture.stats ().count (nano::stat::type::bootstrap_next, nano::stat::detail::next_priority), 1);
}
