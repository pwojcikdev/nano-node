#include <nano/core_test/fakes/work_peer.hpp>
#include <nano/lib/work_version.hpp>
#include <nano/node/node_observers.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/work_generator.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST (work_generator, stopped)
{
	nano::test::system system (1);
	auto & node = *system.nodes[0];
	node.work_generator.stop ();
	std::atomic<bool> done{ false };
	node.work_generator.generate (nano::work_version::work_1, nano::block_hash{}, nano::dev::network_params.work.base, [&done] (std::optional<uint64_t> work) {
		EXPECT_FALSE (work.has_value ());
		done = true;
	});
	ASSERT_TIMELY (5s, done);
}

TEST (work_generator, no_peers)
{
	nano::test::system system (1);
	auto & node = *system.nodes[0];
	nano::block_hash hash{ 1 };
	std::optional<uint64_t> work;
	std::atomic<bool> done{ false };
	auto callback = [&work, &done] (std::optional<uint64_t> work_a) {
		ASSERT_TRUE (work_a.has_value ());
		work = work_a;
		done = true;
	};
	node.work_generator.generate (nano::work_version::work_1, hash, node.network_params.work.base, callback, nano::account{});
	ASSERT_TIMELY (5s, done);
	ASSERT_GE (nano::dev::network_params.work.difficulty (nano::work_version::work_1, hash, *work), node.network_params.work.base);
	// Finished generations are removed automatically
	ASSERT_TIMELY_EQ (5s, node.work_generator.size (), 0);
}

TEST (work_generator, no_peers_disabled)
{
	nano::test::system system;
	nano::node_config node_config = system.default_config ();
	node_config.work_threads = 0;
	auto & node = *system.add_node (node_config);
	ASSERT_FALSE (node.work_generator.enabled ());
	std::atomic<bool> done{ false };
	node.work_generator.generate (nano::work_version::work_1, nano::block_hash{}, nano::dev::network_params.work.base, [&done] (std::optional<uint64_t> work) {
		EXPECT_FALSE (work.has_value ());
		done = true;
	});
	ASSERT_TIMELY (5s, done);
}

TEST (work_generator, no_peers_cancel)
{
	nano::test::system system;
	nano::node_config node_config = system.default_config ();
	node_config.max_work_generate_multiplier = 1e6;
	auto & node = *system.add_node (node_config);
	nano::block_hash hash{ 1 };
	std::atomic<bool> done{ false };
	auto callback_to_cancel = [&done] (std::optional<uint64_t> work) {
		EXPECT_FALSE (work.has_value ());
		done = true;
	};
	node.work_generator.generate (nano::work_version::work_1, hash, nano::difficulty::from_multiplier (1e6, node.network_params.work.base), callback_to_cancel);
	ASSERT_EQ (1, node.work_generator.size ());

	// Manually cancel
	node.work_generator.cancel (hash);
	ASSERT_TIMELY (20s, done && node.work_generator.size () == 0);

	// Now using observer
	done = false;
	node.work_generator.generate (nano::work_version::work_1, hash, nano::difficulty::from_multiplier (1e6, node.network_params.work.base), callback_to_cancel);
	ASSERT_EQ (1, node.work_generator.size ());
	node.observers.work_cancel.notify (hash);
	ASSERT_TIMELY (20s, done && node.work_generator.size () == 0);
}

TEST (work_generator, no_peers_multi)
{
	nano::test::system system (1);
	auto & node = *system.nodes[0];
	nano::block_hash hash{ 1 };
	unsigned total{ 10 };
	std::atomic<unsigned> count{ 0 };
	auto callback = [&count] (std::optional<uint64_t> work_a) {
		ASSERT_TRUE (work_a.has_value ());
		++count;
	};
	// Test many works for the same root
	for (unsigned i{ 0 }; i < total; ++i)
	{
		node.work_generator.generate (nano::work_version::work_1, hash, nano::difficulty::from_multiplier (10, node.network_params.work.base), callback);
	}
	ASSERT_TIMELY_EQ (5s, count, total);
	ASSERT_TIMELY_EQ (5s, node.work_generator.size (), 0);
	count = 0;
	// Test many works for different roots
	for (unsigned i{ 0 }; i < total; ++i)
	{
		nano::block_hash hash_i (i + 1);
		node.work_generator.generate (nano::work_version::work_1, hash_i, node.network_params.work.base, callback);
	}
	ASSERT_TIMELY_EQ (5s, count, total);
	ASSERT_TIMELY_EQ (5s, node.work_generator.size (), 0);
}

TEST (work_generator, peer)
{
	nano::test::system system;
	nano::node_config node_config;
	node_config.peering_port = system.get_available_port ();
	// Disable local work generation
	node_config.work_threads = 0;
	auto node (system.add_node (node_config));
	ASSERT_FALSE (node->work_generator.local_enabled ());
	nano::block_hash hash{ 1 };
	std::optional<uint64_t> work;
	std::atomic<bool> done{ false };
	auto callback = [&work, &done] (std::optional<uint64_t> work_a) {
		ASSERT_TRUE (work_a.has_value ());
		work = work_a;
		done = true;
	};
	auto peer (std::make_shared<fake_work_peer> (node->work, node->io_ctx, system.get_available_port (), fake_work_peer_type::good));
	peer->start ();
	std::vector<nano::work_peer> peers{ { "127.0.0.1", peer->port () } };
	node->work_generator.generate (nano::work_request{ nano::work_version::work_1, hash, node->network_params.work.base, nano::account{}, peers }, callback);
	ASSERT_TIMELY (5s, done);
	ASSERT_GE (nano::dev::network_params.work.difficulty (nano::work_version::work_1, hash, *work), node->network_params.work.base);
	ASSERT_EQ (1, peer->generations_good);
	ASSERT_EQ (0, peer->generations_bad);
	ASSERT_NO_ERROR (system.poll ());
	ASSERT_EQ (0, peer->cancels);
}

// This fails intermittently, the observed behavior is different than what is expected. Disabling because `fake_work_peer` class is not actually used in production.
TEST (work_generator, DISABLED_peer_malicious)
{
	nano::test::system system (1);
	auto node (system.nodes[0]);
	ASSERT_TRUE (node->work_generator.local_enabled ());
	nano::block_hash hash{ 1 };
	std::optional<uint64_t> work;
	std::atomic<bool> done{ false };
	auto callback = [&work, &done] (std::optional<uint64_t> work_a) {
		ASSERT_TRUE (work_a.has_value ());
		work = work_a;
		done = true;
	};
	auto malicious_peer (std::make_shared<fake_work_peer> (node->work, node->io_ctx, system.get_available_port (), fake_work_peer_type::malicious));
	malicious_peer->start ();
	std::vector<nano::work_peer> peers{ { "::ffff:127.0.0.1", malicious_peer->port () } };
	node->work_generator.generate (nano::work_request{ nano::work_version::work_1, hash, node->network_params.work.base, nano::account{}, peers }, callback);
	ASSERT_TIMELY (5s, done);
	ASSERT_GE (nano::dev::network_params.work.difficulty (nano::work_version::work_1, hash, *work), node->network_params.work.base);
	ASSERT_TIMELY (5s, malicious_peer->generations_bad >= 1);
	// make sure it was *not* the malicious peer that replied
	ASSERT_EQ (0, malicious_peer->generations_good);
	// initial generation + the second time when it also starts doing local generation
	// it is possible local work generation finishes before the second request is sent, only 1 failure can be required to pass
	ASSERT_GE (malicious_peer->generations_bad, 1);
	// this peer should not receive a cancel
	ASSERT_EQ (0, malicious_peer->cancels);
	// Test again with no local work generation enabled to make sure the malicious peer is sent more than one request
	node->config.work_threads = 0;
	ASSERT_FALSE (node->work_generator.local_enabled ());
	auto malicious_peer2 (std::make_shared<fake_work_peer> (node->work, node->io_ctx, system.get_available_port (), fake_work_peer_type::malicious));
	malicious_peer2->start ();
	peers[0].port = malicious_peer2->port ();
	node->work_generator.generate (nano::work_request{ nano::work_version::work_1, hash, node->network_params.work.base, nano::account{}, peers }, {});
	ASSERT_TIMELY (5s, malicious_peer2->generations_bad >= 2);
	node->work_generator.cancel (hash);
	ASSERT_EQ (0, malicious_peer2->cancels);
}

// Test disabled because it's failing intermittently.
// PR in which it got disabled: https://github.com/nanocurrency/nano-node/pull/3629
// Issue for investigating it: https://github.com/nanocurrency/nano-node/issues/3630
TEST (work_generator, DISABLED_peer_multi)
{
	nano::test::system system (1);
	auto node (system.nodes[0]);
	ASSERT_TRUE (node->work_generator.local_enabled ());
	nano::block_hash hash{ 1 };
	std::optional<uint64_t> work;
	std::atomic<bool> done{ false };
	auto callback = [&work, &done] (std::optional<uint64_t> work_a) {
		ASSERT_TRUE (work_a.has_value ());
		work = work_a;
		done = true;
	};
	auto good_peer (std::make_shared<fake_work_peer> (node->work, node->io_ctx, system.get_available_port (), fake_work_peer_type::good));
	auto malicious_peer (std::make_shared<fake_work_peer> (node->work, node->io_ctx, system.get_available_port (), fake_work_peer_type::malicious));
	auto slow_peer (std::make_shared<fake_work_peer> (node->work, node->io_ctx, system.get_available_port (), fake_work_peer_type::slow));
	good_peer->start ();
	malicious_peer->start ();
	slow_peer->start ();
	std::vector<nano::work_peer> peers{
		{ "localhost", malicious_peer->port () },
		{ "localhost", slow_peer->port () },
		{ "localhost", good_peer->port () },
	};
	node->work_generator.generate (nano::work_request{ nano::work_version::work_1, hash, node->network_params.work.base, nano::account{}, peers }, callback);
	ASSERT_TIMELY (5s, done);
	ASSERT_GE (nano::dev::network_params.work.difficulty (nano::work_version::work_1, hash, *work), node->network_params.work.base);
	ASSERT_TIMELY_EQ (5s, slow_peer->cancels, 1);
	ASSERT_EQ (0, malicious_peer->generations_good);
	ASSERT_EQ (1, malicious_peer->generations_bad);
	ASSERT_EQ (0, malicious_peer->cancels);

	ASSERT_EQ (0, slow_peer->generations_good);
	ASSERT_EQ (0, slow_peer->generations_bad);
	ASSERT_EQ (1, slow_peer->cancels);

	ASSERT_EQ (1, good_peer->generations_good);
	ASSERT_EQ (0, good_peer->generations_bad);
	ASSERT_EQ (0, good_peer->cancels);
}

TEST (work_generator, fail_resolve)
{
	nano::test::system system (1);
	auto & node = *system.nodes[0];
	nano::block_hash hash{ 1 };
	std::optional<uint64_t> work;
	std::atomic<bool> done{ false };
	auto callback = [&work, &done] (std::optional<uint64_t> work_a) {
		ASSERT_TRUE (work_a.has_value ());
		work = work_a;
		done = true;
	};
	// Unresolvable peers make the local work pool join the race on the retried attempt
	std::vector<nano::work_peer> peers{ { "beeb.boop.123z", 0 } };
	node.work_generator.generate (nano::work_request{ nano::work_version::work_1, hash, node.network_params.work.base, nano::account{}, peers }, callback);
	ASSERT_TIMELY (5s, done);
	ASSERT_GE (nano::dev::network_params.work.difficulty (nano::work_version::work_1, hash, *work), node.network_params.work.base);
}
