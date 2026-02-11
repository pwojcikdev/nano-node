#include <nano/lib/blocks.hpp>
#include <nano/node/transport/fake.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/topology.hpp>
#include <nano/store/ledger_store.hpp>
#include <nano/test_common/chains.hpp>
#include <nano/test_common/random.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <iterator>
#include <map>

using namespace std::chrono_literals;

namespace
{
class responses_helper final
{
public:
	void add (nano::messages::asc_pull_ack const & ack)
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		responses.push_back (ack);
	}

	std::vector<nano::messages::asc_pull_ack> get ()
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return responses;
	}

	std::size_t size ()
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return responses.size ();
	}

	void connect (nano::bootstrap_server & server)
	{
		server.on_response.add ([&] (auto & response, auto & channel) {
			add (response);
		});
	}

private:
	nano::mutex mutex;
	std::vector<nano::messages::asc_pull_ack> responses;
};

/**
 * Checks if both lists contain the same blocks, with `blocks_b` skipped by `skip` elements
 */
bool compare_blocks (auto const & blocks_a, auto const & blocks_b, int skip = 0)
{
	debug_assert (blocks_b.size () >= blocks_a.size () + skip);

	const auto count = blocks_a.size ();
	for (int n = 0; n < count; ++n)
	{
		auto & block_a = *blocks_a[n];
		auto & block_b = *blocks_b[n + skip];

		// nano::block does not have != operator
		if (!(block_a == block_b))
		{
			return false;
		}
	}
	return true;
}

std::deque<std::shared_ptr<nano::block>> expected_topology_blocks (nano::node & node, nano::block_hash start, std::size_t count)
{
	auto txn = node.ledger.tx_begin_read ();
	auto begin = node.store.topology.begin (txn);
	auto end = node.store.topology.end (txn);

	if (!start.is_zero ())
	{
		begin = node.store.topology.begin (txn);
		while (begin != end && begin->first.hash != start)
		{
			++begin;
		}
		if (begin == end)
		{
			return {};
		}
	}

	std::deque<std::shared_ptr<nano::block>> result;
	for (auto it = std::move (begin); it != end && result.size () < count; ++it)
	{
		auto block = node.store.block.get (txn, it->first.hash);
		if (block)
		{
			result.push_back (block);
		}
	}

	return result;
}
}

TEST (bootstrap_server, serve_account_blocks)
{
	nano::test::system system{};
	auto & node = *system.add_node ();

	responses_helper responses;
	responses.connect (node.bootstrap_server);

	auto chains = nano::test::setup_chains (system, node, 1, 128);
	auto [first_account, first_blocks] = chains.front ();

	// Request blocks from account root
	nano::messages::asc_pull_req request{ node.network_params.network };
	request.id = 7;
	request.type = nano::messages::asc_pull_type::blocks;

	nano::messages::asc_pull_req::blocks_payload request_payload{};
	request_payload.start = first_account;
	request_payload.count = nano::bootstrap_server::max_blocks;
	request_payload.start_type = nano::messages::asc_pull_req::hash_type::account;

	request.payload = request_payload;
	request.update_header ();

	node.inbound (request, nano::test::fake_channel (node));

	ASSERT_TIMELY_EQ (5s, responses.size (), 1);

	auto response = responses.get ().front ();
	// Ensure we got response exactly for what we asked for
	ASSERT_EQ (response.id, 7);
	ASSERT_EQ (response.type, nano::messages::asc_pull_type::blocks);

	nano::messages::asc_pull_ack::blocks_payload response_payload;
	ASSERT_NO_THROW (response_payload = std::get<nano::messages::asc_pull_ack::blocks_payload> (response.payload));
	ASSERT_EQ (response_payload.blocks.size (), 128);
	ASSERT_TRUE (compare_blocks (response_payload.blocks, first_blocks));

	// Ensure we don't get any unexpected responses
	ASSERT_ALWAYS (1s, responses.size () == 1);
}

TEST (bootstrap_server, serve_hash)
{
	nano::test::system system{};
	auto & node = *system.add_node ();

	responses_helper responses;
	responses.connect (node.bootstrap_server);

	auto chains = nano::test::setup_chains (system, node, 1, 256);
	auto [account, blocks] = chains.front ();

	// Skip a few blocks to request hash in the middle of the chain
	blocks = nano::block_list_t{ std::next (blocks.begin (), 9), blocks.end () };

	// Request blocks from the middle of the chain
	nano::messages::asc_pull_req request{ node.network_params.network };
	request.id = 7;
	request.type = nano::messages::asc_pull_type::blocks;

	nano::messages::asc_pull_req::blocks_payload request_payload{};
	request_payload.start = blocks.front ()->hash ();
	request_payload.count = nano::bootstrap_server::max_blocks;
	request_payload.start_type = nano::messages::asc_pull_req::hash_type::block;

	request.payload = request_payload;
	request.update_header ();

	node.inbound (request, nano::test::fake_channel (node));

	ASSERT_TIMELY_EQ (5s, responses.size (), 1);

	auto response = responses.get ().front ();
	// Ensure we got response exactly for what we asked for
	ASSERT_EQ (response.id, 7);
	ASSERT_EQ (response.type, nano::messages::asc_pull_type::blocks);

	nano::messages::asc_pull_ack::blocks_payload response_payload;
	ASSERT_NO_THROW (response_payload = std::get<nano::messages::asc_pull_ack::blocks_payload> (response.payload));
	ASSERT_EQ (response_payload.blocks.size (), 128);
	ASSERT_TRUE (compare_blocks (response_payload.blocks, blocks));

	// Ensure we don't get any unexpected responses
	ASSERT_ALWAYS (1s, responses.size () == 1);
}

TEST (bootstrap_server, serve_hash_one)
{
	nano::test::system system{};
	auto & node = *system.add_node ();

	responses_helper responses;
	responses.connect (node.bootstrap_server);

	auto chains = nano::test::setup_chains (system, node, 1, 256);
	auto [account, blocks] = chains.front ();

	// Skip a few blocks to request hash in the middle of the chain
	blocks = nano::block_list_t{ std::next (blocks.begin (), 9), blocks.end () };

	// Request blocks from the middle of the chain
	nano::messages::asc_pull_req request{ node.network_params.network };
	request.id = 7;
	request.type = nano::messages::asc_pull_type::blocks;

	nano::messages::asc_pull_req::blocks_payload request_payload{};
	request_payload.start = blocks.front ()->hash ();
	request_payload.count = 1;
	request_payload.start_type = nano::messages::asc_pull_req::hash_type::block;

	request.payload = request_payload;
	request.update_header ();

	node.inbound (request, nano::test::fake_channel (node));

	ASSERT_TIMELY_EQ (5s, responses.size (), 1);

	auto response = responses.get ().front ();
	// Ensure we got response exactly for what we asked for
	ASSERT_EQ (response.id, 7);
	ASSERT_EQ (response.type, nano::messages::asc_pull_type::blocks);

	nano::messages::asc_pull_ack::blocks_payload response_payload;
	ASSERT_NO_THROW (response_payload = std::get<nano::messages::asc_pull_ack::blocks_payload> (response.payload));
	ASSERT_EQ (response_payload.blocks.size (), 1);
	ASSERT_EQ (response_payload.blocks.front ()->hash (), request_payload.start.as_block_hash ());
}

TEST (bootstrap_server, serve_end_of_chain)
{
	nano::test::system system{};
	auto & node = *system.add_node ();

	responses_helper responses;
	responses.connect (node.bootstrap_server);

	auto chains = nano::test::setup_chains (system, node, 1, 128);
	auto [account, blocks] = chains.front ();

	// Request blocks from account frontier
	nano::messages::asc_pull_req request{ node.network_params.network };
	request.id = 7;
	request.type = nano::messages::asc_pull_type::blocks;

	nano::messages::asc_pull_req::blocks_payload request_payload{};
	request_payload.start = blocks.back ()->hash ();
	request_payload.count = nano::bootstrap_server::max_blocks;
	request_payload.start_type = nano::messages::asc_pull_req::hash_type::block;

	request.payload = request_payload;
	request.update_header ();

	node.inbound (request, nano::test::fake_channel (node));

	ASSERT_TIMELY_EQ (5s, responses.size (), 1);

	auto response = responses.get ().front ();
	// Ensure we got response exactly for what we asked for
	ASSERT_EQ (response.id, 7);
	ASSERT_EQ (response.type, nano::messages::asc_pull_type::blocks);

	nano::messages::asc_pull_ack::blocks_payload response_payload;
	ASSERT_NO_THROW (response_payload = std::get<nano::messages::asc_pull_ack::blocks_payload> (response.payload));
	// Response should contain only the last block from chain
	ASSERT_EQ (response_payload.blocks.size (), 1);
	ASSERT_EQ (*response_payload.blocks.front (), *blocks.back ());
}

TEST (bootstrap_server, serve_missing)
{
	nano::test::system system{};
	auto & node = *system.add_node ();

	responses_helper responses;
	responses.connect (node.bootstrap_server);

	auto chains = nano::test::setup_chains (system, node, 1, 128);

	// Request blocks from account frontier
	nano::messages::asc_pull_req request{ node.network_params.network };
	request.id = 7;
	request.type = nano::messages::asc_pull_type::blocks;

	nano::messages::asc_pull_req::blocks_payload request_payload{};
	request_payload.start = nano::test::random_hash ();
	request_payload.count = nano::bootstrap_server::max_blocks;
	request_payload.start_type = nano::messages::asc_pull_req::hash_type::block;

	request.payload = request_payload;
	request.update_header ();

	node.inbound (request, nano::test::fake_channel (node));

	ASSERT_TIMELY_EQ (5s, responses.size (), 1);

	auto response = responses.get ().front ();
	// Ensure we got response exactly for what we asked for
	ASSERT_EQ (response.id, 7);
	ASSERT_EQ (response.type, nano::messages::asc_pull_type::blocks);

	nano::messages::asc_pull_ack::blocks_payload response_payload;
	ASSERT_NO_THROW (response_payload = std::get<nano::messages::asc_pull_ack::blocks_payload> (response.payload));
	// There should be nothing sent
	ASSERT_EQ (response_payload.blocks.size (), 0);
}

TEST (bootstrap_server, serve_topology)
{
	nano::test::system system{};
	auto & node = *system.add_node ();

	responses_helper responses;
	responses.connect (node.bootstrap_server);

	// Create multiple independent account chains to produce non-trivial topology ordering
	nano::test::setup_chains (system, node, 8, 16);

	nano::messages::asc_pull_req request{ node.network_params.network };
	request.id = 7;
	request.type = nano::messages::asc_pull_type::blocks;

	nano::messages::asc_pull_req::blocks_payload request_payload{};
	request_payload.start = 0;
	request_payload.count = nano::bootstrap_server::max_blocks;
	request_payload.start_type = nano::messages::asc_pull_req::hash_type::topology;

	request.payload = request_payload;
	request.update_header ();

	node.inbound (request, nano::test::fake_channel (node));

	ASSERT_TIMELY_EQ (5s, responses.size (), 1);

	auto response = responses.get ().front ();
	ASSERT_EQ (response.id, 7);
	ASSERT_EQ (response.type, nano::messages::asc_pull_type::blocks);

	nano::messages::asc_pull_ack::blocks_payload response_payload;
	ASSERT_NO_THROW (response_payload = std::get<nano::messages::asc_pull_ack::blocks_payload> (response.payload));

	auto expected = expected_topology_blocks (node, 0, nano::bootstrap_server::max_blocks);
	ASSERT_EQ (response_payload.blocks.size (), expected.size ());
	ASSERT_TRUE (compare_blocks (response_payload.blocks, expected));

	ASSERT_FALSE (response_payload.blocks.empty ());
	auto continuation = response_payload.blocks.back ()->hash ();

	nano::messages::asc_pull_req request2{ node.network_params.network };
	request2.id = 8;
	request2.type = nano::messages::asc_pull_type::blocks;

	nano::messages::asc_pull_req::blocks_payload request2_payload{};
	request2_payload.start = continuation;
	request2_payload.count = nano::bootstrap_server::max_blocks;
	request2_payload.start_type = nano::messages::asc_pull_req::hash_type::topology;

	request2.payload = request2_payload;
	request2.update_header ();

	node.inbound (request2, nano::test::fake_channel (node));

	ASSERT_TIMELY_EQ (5s, responses.size (), 2);

	auto response2 = responses.get ().back ();
	ASSERT_EQ (response2.id, 8);
	ASSERT_EQ (response2.type, nano::messages::asc_pull_type::blocks);

	nano::messages::asc_pull_ack::blocks_payload response2_payload;
	ASSERT_NO_THROW (response2_payload = std::get<nano::messages::asc_pull_ack::blocks_payload> (response2.payload));

	auto expected2 = expected_topology_blocks (node, continuation, nano::bootstrap_server::max_blocks);
	ASSERT_EQ (response2_payload.blocks.size (), expected2.size ());
	ASSERT_TRUE (compare_blocks (response2_payload.blocks, expected2));

	// Response should be inclusive of the start block for continuation requests
	ASSERT_FALSE (response2_payload.blocks.empty ());
	ASSERT_EQ (response2_payload.blocks.front ()->hash (), continuation);
}

TEST (bootstrap_server, serve_multiple)
{
	nano::test::system system{};
	auto & node = *system.add_node ();

	responses_helper responses;
	responses.connect (node.bootstrap_server);

	auto chains = nano::test::setup_chains (system, node, 32, 16);

	{
		// Request blocks from multiple chains at once
		int next_id = 0;
		for (auto & [account, blocks] : chains)
		{
			// Request blocks from account root
			nano::messages::asc_pull_req request{ node.network_params.network };
			request.id = next_id++;
			request.type = nano::messages::asc_pull_type::blocks;

			nano::messages::asc_pull_req::blocks_payload request_payload{};
			request_payload.start = account;
			request_payload.count = nano::bootstrap_server::max_blocks;
			request_payload.start_type = nano::messages::asc_pull_req::hash_type::account;

			request.payload = request_payload;
			request.update_header ();

			node.inbound (request, nano::test::fake_channel (node));
		}
	}

	ASSERT_TIMELY_EQ (15s, responses.size (), chains.size ());

	auto all_responses = responses.get ();
	{
		int next_id = 0;
		for (auto & [account, blocks] : chains)
		{
			// Find matching response
			auto response_it = std::find_if (all_responses.begin (), all_responses.end (), [&] (auto ack) { return ack.id == next_id; });
			ASSERT_TRUE (response_it != all_responses.end ());
			auto response = *response_it;

			// Ensure we got response exactly for what we asked for
			ASSERT_EQ (response.id, next_id);
			ASSERT_EQ (response.type, nano::messages::asc_pull_type::blocks);

			nano::messages::asc_pull_ack::blocks_payload response_payload;
			ASSERT_NO_THROW (response_payload = std::get<nano::messages::asc_pull_ack::blocks_payload> (response.payload));
			ASSERT_EQ (response_payload.blocks.size (), 17); // 1 open block + 16 random blocks
			ASSERT_TRUE (compare_blocks (response_payload.blocks, blocks));

			++next_id;
		}
	}

	// Ensure we don't get any unexpected responses
	ASSERT_ALWAYS (1s, responses.size () == chains.size ());
}

TEST (bootstrap_server, serve_account_info)
{
	nano::test::system system{};
	auto & node = *system.add_node ();

	responses_helper responses;
	responses.connect (node.bootstrap_server);

	auto chains = nano::test::setup_chains (system, node, 1, 128);
	auto [account, blocks] = chains.front ();

	// Request blocks from account root
	nano::messages::asc_pull_req request{ node.network_params.network };
	request.id = 7;
	request.type = nano::messages::asc_pull_type::account_info;

	nano::messages::asc_pull_req::account_info_payload request_payload{};
	request_payload.target = account;
	request_payload.target_type = nano::messages::asc_pull_req::hash_type::account;

	request.payload = request_payload;
	request.update_header ();

	node.inbound (request, nano::test::fake_channel (node));

	ASSERT_TIMELY_EQ (5s, responses.size (), 1);

	auto response = responses.get ().front ();
	// Ensure we got response exactly for what we asked for
	ASSERT_EQ (response.id, 7);
	ASSERT_EQ (response.type, nano::messages::asc_pull_type::account_info);

	nano::messages::asc_pull_ack::account_info_payload response_payload;
	ASSERT_NO_THROW (response_payload = std::get<nano::messages::asc_pull_ack::account_info_payload> (response.payload));

	ASSERT_EQ (response_payload.account, account);
	ASSERT_EQ (response_payload.account_open, blocks.front ()->hash ());
	ASSERT_EQ (response_payload.account_head, blocks.back ()->hash ());
	ASSERT_EQ (response_payload.account_block_count, blocks.size ());
	ASSERT_EQ (response_payload.account_conf_frontier, blocks.back ()->hash ());
	ASSERT_EQ (response_payload.account_conf_height, blocks.size ());

	// Ensure we don't get any unexpected responses
	ASSERT_ALWAYS (1s, responses.size () == 1);
}

TEST (bootstrap_server, serve_account_info_missing)
{
	nano::test::system system{};
	auto & node = *system.add_node ();

	responses_helper responses;
	responses.connect (node.bootstrap_server);

	auto chains = nano::test::setup_chains (system, node, 1, 128);
	auto [account, blocks] = chains.front ();

	// Request blocks from account root
	nano::messages::asc_pull_req request{ node.network_params.network };
	request.id = 7;
	request.type = nano::messages::asc_pull_type::account_info;

	nano::messages::asc_pull_req::account_info_payload request_payload{};
	request_payload.target = nano::test::random_account ();
	request_payload.target_type = nano::messages::asc_pull_req::hash_type::account;

	request.payload = request_payload;
	request.update_header ();

	node.inbound (request, nano::test::fake_channel (node));

	ASSERT_TIMELY_EQ (5s, responses.size (), 1);

	auto response = responses.get ().front ();
	// Ensure we got response exactly for what we asked for
	ASSERT_EQ (response.id, 7);
	ASSERT_EQ (response.type, nano::messages::asc_pull_type::account_info);

	nano::messages::asc_pull_ack::account_info_payload response_payload;
	ASSERT_NO_THROW (response_payload = std::get<nano::messages::asc_pull_ack::account_info_payload> (response.payload));

	ASSERT_EQ (response_payload.account, request_payload.target.as_account ());
	ASSERT_EQ (response_payload.account_open, 0);
	ASSERT_EQ (response_payload.account_head, 0);
	ASSERT_EQ (response_payload.account_block_count, 0);
	ASSERT_EQ (response_payload.account_conf_frontier, 0);
	ASSERT_EQ (response_payload.account_conf_height, 0);

	// Ensure we don't get any unexpected responses
	ASSERT_ALWAYS (1s, responses.size () == 1);
}

TEST (bootstrap_server, serve_frontiers)
{
	nano::test::system system{};
	auto & node = *system.add_node ();

	responses_helper responses;
	responses.connect (node.bootstrap_server);

	auto chains = nano::test::setup_chains (system, node, /* chain count */ 32, /* block count */ 4);

	// Request all frontiers
	nano::messages::asc_pull_req request{ node.network_params.network };
	request.id = 7;
	request.type = nano::messages::asc_pull_type::frontiers;

	nano::messages::asc_pull_req::frontiers_payload request_payload{};
	request_payload.count = nano::bootstrap_server::max_frontiers;
	request_payload.start = 0;

	request.payload = request_payload;
	request.update_header ();

	node.inbound (request, nano::test::fake_channel (node));

	ASSERT_TIMELY_EQ (5s, responses.size (), 1);

	auto response = responses.get ().front ();
	// Ensure we got response exactly for what we asked for
	ASSERT_EQ (response.id, 7);
	ASSERT_EQ (response.type, nano::messages::asc_pull_type::frontiers);

	nano::messages::asc_pull_ack::frontiers_payload response_payload;
	ASSERT_NO_THROW (response_payload = std::get<nano::messages::asc_pull_ack::frontiers_payload> (response.payload));

	ASSERT_EQ (response_payload.frontiers.size (), chains.size () + 1); // +1 for genesis

	// Ensure frontiers match what we expect
	std::map<nano::account, nano::block_hash> expected_frontiers;
	for (auto & [account, blocks] : chains)
	{
		expected_frontiers[account] = blocks.back ()->hash ();
	}
	expected_frontiers[nano::dev::genesis_key.pub] = node.latest (nano::dev::genesis_key.pub);

	for (auto & [account, frontier] : response_payload.frontiers)
	{
		ASSERT_EQ (frontier, expected_frontiers[account]);
		expected_frontiers.erase (account);
	}
	ASSERT_TRUE (expected_frontiers.empty ());
}

TEST (bootstrap_server, serve_frontiers_invalid_count)
{
	nano::test::system system{};
	auto & node = *system.add_node ();

	responses_helper responses;
	responses.connect (node.bootstrap_server);

	auto chains = nano::test::setup_chains (system, node, /* chain count */ 4, /* block count */ 4);

	// Zero count
	{
		nano::messages::asc_pull_req request{ node.network_params.network };
		request.id = 7;
		request.type = nano::messages::asc_pull_type::frontiers;

		nano::messages::asc_pull_req::frontiers_payload request_payload{};
		request_payload.count = 0;
		request_payload.start = 0;

		request.payload = request_payload;
		request.update_header ();

		node.inbound (request, nano::test::fake_channel (node));
	}

	ASSERT_TIMELY_EQ (5s, node.stats.count (nano::stat::type::bootstrap_server, nano::stat::detail::invalid), 1);

	// Count larger than allowed
	{
		nano::messages::asc_pull_req request{ node.network_params.network };
		request.id = 7;
		request.type = nano::messages::asc_pull_type::frontiers;

		nano::messages::asc_pull_req::frontiers_payload request_payload{};
		request_payload.count = nano::bootstrap_server::max_frontiers + 1;
		request_payload.start = 0;

		request.payload = request_payload;
		request.update_header ();

		node.inbound (request, nano::test::fake_channel (node));
	}

	ASSERT_TIMELY_EQ (5s, node.stats.count (nano::stat::type::bootstrap_server, nano::stat::detail::invalid), 2);

	// Max numeric value
	{
		nano::messages::asc_pull_req request{ node.network_params.network };
		request.id = 7;
		request.type = nano::messages::asc_pull_type::frontiers;

		nano::messages::asc_pull_req::frontiers_payload request_payload{};
		request_payload.count = std::numeric_limits<decltype (request_payload.count)>::max ();
		request_payload.start = 0;

		request.payload = request_payload;
		request.update_header ();

		node.inbound (request, nano::test::fake_channel (node));
	}

	ASSERT_TIMELY_EQ (5s, node.stats.count (nano::stat::type::bootstrap_server, nano::stat::detail::invalid), 3);

	// Ensure we don't get any unexpected responses
	ASSERT_ALWAYS (1s, responses.size () == 0);
}
