#include <nano/node/network.hpp>
#include <nano/node/network_reachout.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace std::chrono_literals;

/*
 * The reachout flags keep every reachout loop from starting, while a node without them runs all three
 */
TEST (network_reachout, disabled)
{
	nano::test::system system;
	nano::node_flags flags;
	flags.disable_reachout = true;
	flags.disable_reachout_preconfigured = true;
	auto & node = *system.add_node (flags);
	auto & control = *system.make_disconnected_node ();

	// The control node, started second, has been round each loop at least once
	ASSERT_TIMELY (5s, control.stats.count (nano::stat::type::network, nano::stat::detail::loop_reachout) >= 1);
	ASSERT_TIMELY (5s, control.stats.count (nano::stat::type::network, nano::stat::detail::loop_reachout_cached) >= 1);
	ASSERT_TIMELY (5s, control.stats.count (nano::stat::type::network, nano::stat::detail::loop_reachout_preconfigured) >= 1);

	// The flagged node, started first, has been round none
	ASSERT_EQ (0, node.stats.count (nano::stat::type::network, nano::stat::detail::loop_reachout));
	ASSERT_EQ (0, node.stats.count (nano::stat::type::network, nano::stat::detail::loop_reachout_cached));
	ASSERT_EQ (0, node.stats.count (nano::stat::type::network, nano::stat::detail::loop_reachout_preconfigured));
}

/*
 * A reachout resolves the address and connects to it, and a reachout to a peer already connected sends it a keepalive instead
 */
TEST (network_reachout, resolve_and_connect)
{
	nano::test::system system;
	// A long keepalive period leaves the keepalive sent on connect and the reachout's as the only ones
	nano::node_config node_config = system.default_config ();
	node_config.network_params.network.keepalive_period = 10min;
	auto & node = *system.add_node (node_config);
	nano::node_config peer_config = system.default_config ();
	peer_config.network_params.network.keepalive_period = 10min;
	auto peer = system.make_disconnected_node (peer_config, nano::node_flags{});
	auto const port = peer->network.port.load ();
	auto const received = [&peer] () {
		return peer->stats.count (nano::stat::type::message, nano::stat::detail::keepalive, nano::stat::dir::in);
	};

	// The first reachout connects
	node.network_reachout.reachout ("::1", port);
	ASSERT_TIMELY (5s, node.network.find_node_id (peer->get_node_id ()));
	ASSERT_TIMELY_EQ (5s, 1, received ());

	// A reachout to the connected peer sends a keepalive down the existing channel and opens no second one
	node.network_reachout.reachout ("::1", port);
	ASSERT_TIMELY_EQ (5s, 2, received ());
	ASSERT_EQ (1, node.network.size ());
}

/*
 * A reachout trigger wakes the reachout loop and no network loop, so neither keepalive rounds nor cleanup rounds follow the rep crawler's cadence
 */
TEST (network_reachout, trigger_wakes_only_its_loop)
{
	nano::test::system system;
	// Without the rep crawler the test is the only source of triggers, and the long keepalive period keeps that loop from ticking on its own
	nano::node_flags flags;
	flags.disable_rep_crawler = true;
	nano::node_config node_config = system.default_config ();
	node_config.network_params.network.keepalive_period = 10min;
	auto & node = *system.add_node (node_config, flags);
	auto const count = [&node] (nano::stat::detail detail) {
		return node.stats.count (nano::stat::type::network, detail);
	};
	auto const cleanup_before = count (nano::stat::detail::loop_cleanup);
	auto const preconfigured_before = count (nano::stat::detail::loop_reachout_preconfigured);

	// A hundred triggers, each landing while the loops wait
	for (auto i = 0; i < 100; ++i)
	{
		node.network_reachout.trigger ();
		std::this_thread::sleep_for (10ms);
	}
	ASSERT_EQ (100, count (nano::stat::detail::trigger_reachout));

	// The reachout loop went round, the keepalive loop never did, and the cleanup loop kept to its one tick a second
	ASSERT_TIMELY (5s, count (nano::stat::detail::loop_reachout_preconfigured) > preconfigured_before);
	ASSERT_ALWAYS_EQ (500ms, 0, count (nano::stat::detail::loop_keepalive));
	ASSERT_LE (count (nano::stat::detail::loop_cleanup), cleanup_before + 3);
}

/*
 * Reachout triggers do not age the duplicate filter, which keeps counting its cutoff in seconds
 */
TEST (network_reachout, trigger_keeps_filter_epoch)
{
	nano::test::system system;
	nano::node_flags flags;
	flags.disable_rep_crawler = true;
	nano::node_config node_config = system.default_config ();
	node_config.network->duplicate_filter_cutoff = 3;
	auto & node = *system.add_node (node_config, flags);
	std::vector<uint8_t> const bytes (32, 1);
	ASSERT_FALSE (node.network.filter.apply (bytes.data (), bytes.size ()));

	// Fifty triggers in half a second leave the entry in place, where fifty cleanup rounds would have aged it out
	for (auto i = 0; i < 50; ++i)
	{
		node.network_reachout.trigger ();
		std::this_thread::sleep_for (10ms);
	}
	ASSERT_TRUE (node.network.filter.check (bytes.data (), bytes.size ()));

	// It still expires with the cutoff, counted in real seconds
	ASSERT_TIMELY (10s, !node.network.filter.check (bytes.data (), bytes.size ()));
}

/*
 * A reachout trigger makes the preconfigured loop reach out at once instead of at its next period
 */
TEST (network_reachout, trigger_reaches_out)
{
	nano::test::system system;
	nano::node_flags flags;
	flags.disable_rep_crawler = true;
	nano::node_config node_config = system.default_config ();
	// A peer address that resolves, with nothing listening on the default port, and periods too long to fire during the test
	node_config.preconfigured_peers = { "::1" };
	node_config.network_params.network.reachout_preconfigured_period = 10min;
	node_config.network_params.network.reachout_preconfigured_warmup_period = 10min;
	auto & node = *system.add_node (node_config, flags);
	auto const count = [&node] () {
		return node.stats.count (nano::stat::type::network, nano::stat::detail::reachout_preconfigured);
	};

	// The first round reaches out once and the next waits for the period
	ASSERT_TIMELY_EQ (5s, 1, count ());
	ASSERT_ALWAYS_EQ (1s, 1, count ());

	// A trigger brings the next round forward
	node.network_reachout.trigger ();
	ASSERT_TIMELY_EQ (5s, 2, count ());
}

/*
 * A stop joins the reachout tasks the loop has not purged yet, so the node shuts down with reachouts still listed
 */
TEST (network_reachout, stop_with_tasks)
{
	nano::test::system system;
	// Without the rep crawler nothing triggers the loop, and the long merge period keeps it from ticking, so the finished task is never purged and is still listed at stop
	nano::node_flags flags;
	flags.disable_rep_crawler = true;
	nano::node_config node_config = system.default_config ();
	node_config.network_params.network.merge_period = 10min;
	auto & node = *system.add_node (node_config, flags);
	auto peer = system.make_disconnected_node ();

	node.network_reachout.reachout ("::1", peer->network.port.load ());
	ASSERT_TIMELY (5s, node.network.find_node_id (peer->get_node_id ()));

	// The stop returns with the task joined and the channel closed
	node.stop ();
	ASSERT_TRUE (node.stopped);
	ASSERT_EQ (0, node.network.size ());
}
