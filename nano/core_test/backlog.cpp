#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/files.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/backlog_scan.hpp>
#include <nano/node/bounded_backlog.hpp>
#include <nano/node/ledger_notifications.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/test_common/chains.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <unordered_set>

using namespace std::chrono_literals;

/*
 * Ensures all not confirmed accounts get activated by backlog scan periodically
 */
TEST (backlog, population)
{
	nano::mutex mutex;
	std::unordered_set<nano::account> activated;

	nano::test::system system{};
	auto & node = *system.add_node ();

	node.backlog_scan.batch_activated.add ([&] (auto const & batch) {
		nano::lock_guard<nano::mutex> lock{ mutex };
		for (auto const & info : batch)
		{
			activated.insert (info.account);
		}
	});

	auto blocks = nano::test::setup_independent_blocks (system, node, 256);

	// Checks if `activated` set contains all accounts we previously set up
	auto all_activated = [&] () {
		nano::lock_guard<nano::mutex> lock{ mutex };
		return std::all_of (blocks.begin (), blocks.end (), [&] (auto const & item) {
			return activated.count (item->account ()) != 0;
		});
	};
	ASSERT_TIMELY (5s, all_activated ());

	// Clear activated set to ensure we activate those accounts more than once
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		activated.clear ();
	}

	ASSERT_TIMELY (5s, all_activated ());
}

/*
 * Ensures that elections are activated without live traffic
 */
TEST (backlog, election_activation)
{
	nano::test::system system;
	nano::node_config node_config = system.default_config ();
	auto & node = *system.add_node (node_config);
	nano::keypair key;
	nano::block_builder builder;
	auto send = builder
				.state ()
				.account (nano::dev::genesis_key.pub)
				.previous (nano::dev::genesis->hash ())
				.representative (nano::dev::genesis_key.pub)
				.balance (nano::dev::constants.genesis_amount - nano::Knano_ratio)
				.link (key.pub)
				.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
				.work (*node.work_generate_blocking (nano::dev::genesis->hash ()))
				.build ();
	{
		auto transaction = node.ledger.tx_begin_write ();
		ASSERT_EQ (nano::block_status::progress, node.ledger.process (transaction, send));
	}
	ASSERT_TIMELY_EQ (5s, node.active.size (), 1);
}

TEST (backlog, idle_bounded_backlog_scan)
{
	for (auto scan_rate : { 1u, 0u })
	{
		SCOPED_TRACE (scan_rate);
		nano::test::system system;
		auto config = system.default_config ();
		config.bounded_backlog->scan_rate = scan_rate;
		config.backlog_scan->enable = false;
		auto & node = *system.add_node (config);

		// Waiting for the first entry must release the index lock even when the limiter is disabled.
		ASSERT_ALWAYS_EQ (250ms, 0, node.stats.count (nano::stat::type::bounded_backlog, nano::stat::detail::loop_scan));

		auto const blocks = nano::test::setup_chain (system, node, 1, nano::dev::genesis_key, false);
		node.backlog_scan.trigger ();
		ASSERT_TIMELY (5s, node.stats.count (nano::stat::type::bounded_backlog, nano::stat::detail::scanned) > 0);

		nano::test::confirm (node.ledger, blocks);
		node.backlog_scan.trigger ();
		ASSERT_TIMELY_EQ (5s, node.backlog.index_size (), 0);
	}
}

TEST (backlog, rollback_batch_spans_priority_buckets)
{
	nano::test::system system;
	auto config = system.default_config ();
	config.backlog_scan->enable = false;
	config.max_backlog = 1;
	config.bounded_backlog->batch_size = 1;
	nano::node_flags flags;
	flags.disable_elections = true;
	auto node = std::make_shared<nano::node> (nano::unique_path (), config, system.work, flags);
	system.register_node (node);

	// Leave one independent open in each of three different priority buckets.
	for (nano::uint128_t const amount : { nano::uint128_t{ 1 }, nano::nano_ratio, nano::Knano_ratio })
	{
		nano::keypair destination;
		nano::test::setup_new_account (system, *node, amount, nano::dev::genesis_key, destination, destination.pub, false);
	}
	nano::test::confirm (node->ledger, node->latest (nano::dev::genesis_key.pub));
	ASSERT_EQ (3, node->ledger.backlog_size ());

	// Start rollbacks after the scanner has indexed all three eligible accounts.
	node->backlog_scan.start ();
	node->backlog_scan.trigger ();
	ASSERT_TIMELY_EQ (5s, 3, node->backlog.index_size ());
	node->ledger_notifications.start ();
	node->backlog.start ();
	ASSERT_TIMELY_EQ (5s, 1, node->ledger.backlog_size ());
	ASSERT_EQ (2, node->stats.count (nano::stat::type::bounded_backlog, nano::stat::detail::performing_rollbacks));
}
