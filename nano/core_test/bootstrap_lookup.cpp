#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/stats.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/block_rebroadcaster.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/bootstrap_server.hpp>
#include <nano/node/bootstrap/bootstrap_service.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/vote_cache.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/vote.hpp>
#include <nano/test_common/random.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>

using namespace std::chrono_literals;

namespace
{
// Config of a node that only serves blocks: it floods none and pulls none, so a peer gets a block from it by request alone
nano::node_config server_config (nano::test::system & system)
{
	auto config = system.default_config ();
	config.block_rebroadcaster->enable = false;
	config.bootstrap->enable = false;
	return config;
}

// Config of a node that learns of an account only through a lookup: the strategies that discover accounts on their own are off,
// while the priority strategy that pulls a looked up account and the dependency strategy that looks it up run
nano::node_config client_config (nano::test::system & system)
{
	auto config = system.default_config ();
	config.block_rebroadcaster->enable = false;
	config.bootstrap->enable_database_scan = false;
	config.bootstrap->enable_frontier_scan = false;
	config.bootstrap->enable_topo_scan = false;
	config.bootstrap->lookup.retry_delay = 50ms;
	return config;
}

// A send from the genesis account to `destination`, leaving the genesis account with `balance`
std::shared_ptr<nano::block> make_genesis_send (nano::test::system & system, nano::uint128_t const & balance, nano::account const & destination)
{
	nano::state_block_builder builder;
	return builder.make_block ()
	.account (nano::dev::genesis_key.pub)
	.previous (nano::dev::genesis->hash ())
	.representative (nano::dev::genesis_key.pub)
	.link (destination)
	.balance (balance)
	.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
	.work (*system.work.generate (nano::dev::genesis->hash ()))
	.build ();
}

// The first block of `key`'s account, receiving `amount` from `send`
std::shared_ptr<nano::block> make_open (nano::test::system & system, nano::keypair const & key, std::shared_ptr<nano::block> const & send, nano::uint128_t const & amount)
{
	nano::state_block_builder builder;
	return builder.make_block ()
	.account (key.pub)
	.previous (0)
	.representative (key.pub)
	.link (send->hash ())
	.balance (amount)
	.sign (key.prv, key.pub)
	.work (*system.work.generate (key.pub))
	.build ();
}

// A send from `key`'s account on top of `previous`, leaving `balance`
std::shared_ptr<nano::block> make_send (nano::test::system & system, nano::keypair const & key, std::shared_ptr<nano::block> const & previous, nano::uint128_t const & balance)
{
	nano::state_block_builder builder;
	return builder.make_block ()
	.account (key.pub)
	.previous (previous->hash ())
	.representative (key.pub)
	.link (nano::dev::genesis_key.pub)
	.balance (balance)
	.sign (key.prv, key.pub)
	.work (*system.work.generate (previous->hash ()))
	.build ();
}

// The blocks of a test: a send from genesis to a fresh account, the account's open and a send that follows it
struct test_chain
{
	nano::keypair key;
	std::shared_ptr<nano::block> genesis_send;
	std::shared_ptr<nano::block> open;
	std::shared_ptr<nano::block> send;

	explicit test_chain (nano::test::system & system) :
		genesis_send{ make_genesis_send (system, nano::dev::constants.genesis_amount - 100, key.pub) },
		open{ make_open (system, key, genesis_send, 100) },
		send{ make_send (system, key, open, 50) }
	{
	}
};
}

/*
 * Looking up a missing block that opens an account makes the node pull that account, which brings the block.
 * Only the serving node holds the account, and nothing but the lookup tells the other node that it exists.
 */
TEST (bootstrap_lookup, pull_unopened_account)
{
	nano::test::system system;
	auto & server = *system.add_node (server_config (system));
	auto & client = *system.add_node (client_config (system));
	test_chain chain{ system };

	// Both nodes hold the send, only the serving node holds the account it funds
	ASSERT_EQ (nano::block_status::progress, server.process (chain.genesis_send));
	ASSERT_EQ (nano::block_status::progress, server.process (chain.open));
	ASSERT_EQ (nano::block_status::progress, client.process (chain.genesis_send));

	// Without a lookup the account never arrives
	ASSERT_ALWAYS (500ms, client.block (chain.open->hash ()) == nullptr);
	ASSERT_EQ (0, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::request));

	client.bootstrap.lookup (chain.open->hash ());

	// The account is named by the first lookup and pulled
	ASSERT_TIMELY (5s, client.block (chain.open->hash ()) != nullptr);
	ASSERT_EQ (1, client.stats.count (nano::stat::type::bootstrap_lookup_insert, nano::stat::detail::inserted));
	ASSERT_EQ (1, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::request));
	ASSERT_EQ (1, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::resolved));
}

/*
 * The common case: a node holds an account but missed its latest block. Looking the block up pulls the account from the node's
 * frontier, which brings the block.
 */
TEST (bootstrap_lookup, pull_from_frontier)
{
	nano::test::system system;
	auto & server = *system.add_node (server_config (system));
	auto & client = *system.add_node (client_config (system));
	test_chain chain{ system };

	// Both nodes hold the account, only the serving node holds its latest block
	for (auto const & block : { chain.genesis_send, chain.open, chain.send })
	{
		ASSERT_EQ (nano::block_status::progress, server.process (block));
	}
	ASSERT_EQ (nano::block_status::progress, client.process (chain.genesis_send));
	ASSERT_EQ (nano::block_status::progress, client.process (chain.open));

	client.bootstrap.lookup (chain.send->hash ());

	// The latest block arrives after a single lookup
	ASSERT_TIMELY (5s, client.block (chain.send->hash ()) != nullptr);
	ASSERT_EQ (1, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::request));
	ASSERT_EQ (1, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::resolved));
}

/*
 * A block that is in the ledger by the time its lookup is due needs no lookup.
 */
TEST (bootstrap_lookup, block_in_ledger)
{
	nano::test::system system;
	system.add_node (server_config (system));
	auto & client = *system.add_node (client_config (system));
	test_chain chain{ system };

	ASSERT_EQ (nano::block_status::progress, client.process (chain.genesis_send));

	client.bootstrap.lookup (chain.genesis_send->hash ());

	// The block is dropped without a request
	ASSERT_TIMELY_EQ (5s, 1, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::old));
	ASSERT_EQ (0, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::request));
}

/*
 * A lookup that a peer cannot answer is retried, and reaches a peer that knows the block once one joins.
 * The node starts with a single peer that lacks the block; the peer that holds it joins after the first one has answered with nothing.
 */
TEST (bootstrap_lookup, retry_on_another_peer)
{
	nano::test::system system;
	system.add_node (server_config (system));
	auto config = client_config (system);
	config.bootstrap->lookup.max_retries = 10;
	auto & client = *system.add_node (config);
	test_chain chain{ system };

	ASSERT_EQ (nano::block_status::progress, client.process (chain.genesis_send));

	client.bootstrap.lookup (chain.open->hash ());

	// The only peer does not know the block
	ASSERT_TIMELY (5s, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::empty) >= 1);
	ASSERT_GE (client.stats.count (nano::stat::type::bootstrap_lookup_fail, nano::stat::detail::retry), 1);

	// A peer that holds the account joins the network
	auto & holding = *system.add_node (server_config (system));
	ASSERT_EQ (nano::block_status::progress, holding.process (chain.genesis_send));
	ASSERT_EQ (nano::block_status::progress, holding.process (chain.open));

	// One of the remaining retries reaches it and the account is pulled
	ASSERT_TIMELY (10s, client.block (chain.open->hash ()) != nullptr);
	ASSERT_EQ (1, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::resolved));
	ASSERT_EQ (0, client.stats.count (nano::stat::type::bootstrap_lookup_fail, nano::stat::detail::exhausted));
}

/*
 * A block that no peer knows is looked up once more for every retry, then forgotten, so that a later lookup starts over.
 */
TEST (bootstrap_lookup, give_up_after_retries)
{
	nano::test::system system;
	system.add_node (server_config (system));
	auto config = client_config (system);
	config.bootstrap->lookup.max_retries = 2;
	auto & client = *system.add_node (config);

	// A hash that belongs to no block anywhere
	auto const hash = nano::test::random_hash ();

	client.bootstrap.lookup (hash);

	// The first lookup and two retries, each answered with nothing, after which the block is forgotten
	ASSERT_TIMELY_EQ (5s, 1, client.stats.count (nano::stat::type::bootstrap_lookup_fail, nano::stat::detail::exhausted));
	ASSERT_EQ (3, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::request));
	ASSERT_EQ (3, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::empty));
	ASSERT_EQ (2, client.stats.count (nano::stat::type::bootstrap_lookup_fail, nano::stat::detail::retry));

	// No further lookup goes out on its own
	ASSERT_ALWAYS_EQ (500ms, 3, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::request));

	// Looking it up again starts a new round
	client.bootstrap.lookup (hash);
	ASSERT_EQ (2, client.stats.count (nano::stat::type::bootstrap_lookup_insert, nano::stat::detail::inserted));
	ASSERT_TIMELY_EQ (5s, 4, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::request));
}

/*
 * A lookup that gets no answer fails once the request times out, and counts against the retries like an empty answer.
 */
TEST (bootstrap_lookup, timeout)
{
	nano::test::system system;
	auto silent_config = server_config (system);
	silent_config.bootstrap_server->enable = false;
	auto & server = *system.add_node (silent_config);
	auto config = client_config (system);
	config.bootstrap->request_timeout = 500ms;
	config.bootstrap->lookup.max_retries = 1;
	auto & client = *system.add_node (config);
	test_chain chain{ system };

	// The silent node holds the account but answers no request
	ASSERT_EQ (nano::block_status::progress, server.process (chain.genesis_send));
	ASSERT_EQ (nano::block_status::progress, server.process (chain.open));
	ASSERT_EQ (nano::block_status::progress, client.process (chain.genesis_send));

	client.bootstrap.lookup (chain.open->hash ());

	// Both lookups time out and the block is forgotten
	ASSERT_TIMELY_EQ (10s, 1, client.stats.count (nano::stat::type::bootstrap_lookup_fail, nano::stat::detail::exhausted));
	ASSERT_EQ (2, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::request));
	ASSERT_EQ (2, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::timeout));
	ASSERT_EQ (nullptr, client.block (chain.open->hash ()));
}

/*
 * With the dependency walker off nothing would serve a lookup, so it is ignored.
 */
TEST (bootstrap_lookup, disabled)
{
	nano::test::system system;
	auto & server = *system.add_node (server_config (system));
	auto config = client_config (system);
	config.bootstrap->enable_dependency_walker = false;
	auto & client = *system.add_node (config);
	test_chain chain{ system };

	ASSERT_EQ (nano::block_status::progress, server.process (chain.genesis_send));
	ASSERT_EQ (nano::block_status::progress, server.process (chain.open));
	ASSERT_EQ (nano::block_status::progress, client.process (chain.genesis_send));

	client.bootstrap.lookup (chain.open->hash ());

	// The block is not tracked and never arrives
	ASSERT_EQ (0, client.stats.count (nano::stat::type::bootstrap_lookup_insert, nano::stat::detail::inserted));
	ASSERT_ALWAYS (500ms, client.block (chain.open->hash ()) == nullptr);
	ASSERT_EQ (0, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::request));
}

/*
 * Resetting bootstrap forgets the blocks being looked up.
 */
TEST (bootstrap_lookup, reset)
{
	nano::test::system system;
	system.add_node (server_config (system));
	auto config = client_config (system);
	config.bootstrap->lookup.retry_delay = 1h;
	auto & client = *system.add_node (config);

	// A block nobody knows waits a long time for its retry
	auto const hash = nano::test::random_hash ();
	client.bootstrap.lookup (hash);
	ASSERT_TIMELY_EQ (5s, 1, client.stats.count (nano::stat::type::bootstrap_lookup_fail, nano::stat::detail::retry));

	// Meanwhile it is still tracked
	client.bootstrap.lookup (hash);
	ASSERT_EQ (1, client.stats.count (nano::stat::type::bootstrap_lookup_insert, nano::stat::detail::duplicate));

	// After a reset it is not, and is looked up again at once
	client.bootstrap.reset ();
	client.bootstrap.lookup (hash);
	ASSERT_EQ (2, client.stats.count (nano::stat::type::bootstrap_lookup_insert, nano::stat::detail::inserted));
	ASSERT_TIMELY_EQ (5s, 2, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::request));
}

/*
 * A lookup answered with a payload of the wrong type counts as failed and is retried.
 * The serving node's first account info response reaches the other node as a frontiers payload under the same id, ahead of the genuine one.
 */
TEST (bootstrap_lookup, wrong_answer_retried)
{
	// Declared ahead of the system, so that the server's observer never outlives it
	std::atomic<bool> forged{ false };

	nano::test::system system;
	auto & server = *system.add_node (server_config (system));
	auto & client = *system.add_node (client_config (system));
	test_chain chain{ system };

	ASSERT_EQ (nano::block_status::progress, server.process (chain.genesis_send));
	ASSERT_EQ (nano::block_status::progress, server.process (chain.open));
	ASSERT_EQ (nano::block_status::progress, client.process (chain.genesis_send));

	// The first account info response the server prepares reaches the client with a payload of the wrong type
	server.bootstrap_server.on_response.add ([&client, &forged] (nano::messages::asc_pull_ack const & response, auto const &) {
		if (response.type != nano::messages::asc_pull_type::account_info)
		{
			return;
		}
		if (forged.exchange (true))
		{
			return;
		}
		nano::messages::asc_pull_ack message{ client.network_params.network };
		message.id = response.id;
		message.type = nano::messages::asc_pull_type::frontiers;
		message.payload = nano::messages::asc_pull_ack::frontiers_payload{};
		message.update_header ();
		client.bootstrap.process (message, nano::test::fake_channel (client));
	});

	client.bootstrap.lookup (chain.open->hash ());

	// The forged answer fails the first lookup, the retry gets a genuine one and the account is pulled
	ASSERT_TIMELY (5s, client.block (chain.open->hash ()) != nullptr);
	ASSERT_EQ (1, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::invalid));
	ASSERT_EQ (1, client.stats.count (nano::stat::type::bootstrap_lookup_fail, nano::stat::detail::retry));
	ASSERT_EQ (2, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::request));
	ASSERT_EQ (1, client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::resolved));
}

/*
 * With bootstrap turned off as a whole nothing would serve a lookup either, so it is ignored.
 */
TEST (bootstrap_lookup, bootstrap_disabled)
{
	nano::test::system system;
	auto config = client_config (system);
	config.bootstrap->enable = false;
	auto & client = *system.add_node (config);

	client.bootstrap.lookup (nano::test::random_hash ());

	ASSERT_EQ (0, client.stats.count (nano::stat::type::bootstrap_lookup_insert, nano::stat::detail::inserted));
}

/*
 * Votes for a block that never reached the node lead to the block: the hinted scan finds it missing and has it looked up.
 * The votes go straight into the vote cache, so the scan is the only thing that knows of the block.
 */
TEST (bootstrap_lookup, hinted_scan_looks_up_missing_block)
{
	nano::test::system system;
	auto & server = *system.add_node (server_config (system));
	auto & client = *system.add_node (client_config (system));
	test_chain chain{ system };

	// Only the serving node holds the account
	ASSERT_EQ (nano::block_status::progress, server.process (chain.genesis_send));
	ASSERT_EQ (nano::block_status::progress, server.process (chain.open));
	ASSERT_EQ (nano::block_status::progress, client.process (chain.genesis_send));

	// Enough vote weight for the missing block reaches the vote cache
	auto const vote = nano::test::make_final_vote (nano::dev::genesis_key, std::vector<nano::block_hash>{ chain.open->hash () });
	client.vote_cache.insert (vote, nano::dev::constants.genesis_amount);

	// The scan finds the block missing, its account is looked up and pulled
	ASSERT_TIMELY (5s, client.stats.count (nano::stat::type::hinting, nano::stat::detail::missing_block) >= 1);
	ASSERT_TIMELY (5s, client.block (chain.open->hash ()) != nullptr);
	ASSERT_GE (client.stats.count (nano::stat::type::bootstrap_lookup, nano::stat::detail::resolved), 1);
}

/*
 * A pruned block is missing from the ledger too, but the node had it once, so votes for it start no lookup.
 */
TEST (bootstrap_lookup, pruned_block_not_looked_up)
{
	nano::test::system system;
	system.add_node (server_config (system));
	nano::node_flags flags;
	flags.enable_pruning = true;
	flags.disable_topo_index = true; // Topo index is incompatible with pruning
	auto config = client_config (system);
	config.enable_voting = false; // A pruning node refuses to start with voting enabled
	auto & client = *system.add_node (config, flags);
	test_chain chain{ system };

	// The send is confirmed and pruned behind a block that follows it
	ASSERT_EQ (nano::block_status::progress, client.process (chain.genesis_send));
	nano::state_block_builder builder;
	auto const successor = builder.make_block ()
						   .account (nano::dev::genesis_key.pub)
						   .previous (chain.genesis_send->hash ())
						   .representative (nano::dev::genesis_key.pub)
						   .link (chain.key.pub)
						   .balance (nano::dev::constants.genesis_amount - 200)
						   .sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
						   .work (*system.work.generate (chain.genesis_send->hash ()))
						   .build ();
	ASSERT_EQ (nano::block_status::progress, client.process (successor));
	nano::test::confirm (client.ledger, successor);
	{
		auto transaction = client.ledger.tx_begin_write ();
		ASSERT_EQ (1, client.ledger.pruning_action (transaction, chain.genesis_send->hash (), 1));
	}

	// Votes for the pruned block reach the vote cache
	auto const vote = nano::test::make_final_vote (nano::dev::genesis_key, std::vector<nano::block_hash>{ chain.genesis_send->hash () });
	client.vote_cache.insert (vote, nano::dev::constants.genesis_amount);

	// The scan finds it missing from the ledger but does not have it looked up
	ASSERT_TIMELY (5s, client.stats.count (nano::stat::type::hinting, nano::stat::detail::missing_block) >= 1);
	ASSERT_ALWAYS_EQ (500ms, 0, client.stats.count (nano::stat::type::bootstrap_lookup_insert, nano::stat::detail::inserted));
}
