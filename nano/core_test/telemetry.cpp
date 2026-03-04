#include <nano/lib/stream.hpp>
#include <nano/node/telemetry.hpp>
#include <nano/node/transport/fake.hpp>
#include <nano/test_common/network.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/telemetry.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/endian/conversion.hpp>

using namespace std::chrono_literals;

TEST (telemetry, signatures)
{
	nano::keypair node_id;
	nano::messages::telemetry_data data;
	data.node_id = node_id.pub;
	data.major_version = 20;
	data.minor_version = 1;
	data.patch_version = 5;
	data.pre_release_version = 2;
	data.maker = 1;
	data.database_backend = static_cast<uint8_t> (nano::messages::telemetry_database_backend::lmdb);
	data.confirmation_latency_ms = 500;
	data.bootstrap_status = static_cast<uint8_t> (nano::messages::telemetry_bootstrap_status::synced);
	data.timestamp = std::chrono::system_clock::time_point (100ms);

	nano::messages::telemetry_ack ack{ nano::dev::network_params.network, data };
	ack.sign (node_id);
	ASSERT_TRUE (ack.validate_signature ());
	auto signature = ack.signature;

	// Check that the signature is different if changing a piece of data
	data.maker = 2;
	nano::messages::telemetry_ack ack2{ nano::dev::network_params.network, data };
	ack2.sign (node_id);
	ASSERT_NE (ack2.signature, signature);
}

// Old node (v1 data payload) -> New node: signature must validate, new fields default to 0
TEST (telemetry, backward_compat_v1_to_new)
{
	nano::keypair node_id;
	nano::messages::telemetry_data data;
	data.node_id = node_id.pub;
	data.major_version = 20;
	data.minor_version = 1;
	data.patch_version = 5;
	data.pre_release_version = 2;
	data.maker = 1;
	data.timestamp = std::chrono::system_clock::time_point (100ms);
	data.active_difficulty = 0xffffffc000000000;
	data.version = nano::messages::telemetry_version::v1;

	// Simulate old node: serialize v1 data fields only
	std::vector<uint8_t> data_payload;
	{
		nano::vectorstream stream (data_payload);
		data.serialize (stream);
	}
	ASSERT_EQ (data_payload.size (), nano::messages::telemetry_data::size_v1);

	// Sign the data payload
	auto sig = nano::sign_message (node_id.prv, node_id.pub, data_payload.data (), data_payload.size ());

	// Build full wire payload: [signature][data]
	std::vector<uint8_t> full_payload;
	{
		nano::vectorstream stream (full_payload);
		nano::write (stream, sig);
		nano::write (stream, data_payload);
	}

	// Build a telemetry_ack header with the correct payload size
	nano::messages::message_header hdr (nano::dev::network_params.network, nano::messages::message_type::telemetry_ack);
	hdr.extensions &= ~nano::messages::message_header::telemetry_size_mask;
	hdr.extensions |= std::bitset<16> (static_cast<unsigned long long> (full_payload.size ()));

	bool error = false;
	nano::bufferstream stream (full_payload.data (), full_payload.size ());
	nano::messages::telemetry_ack received (error, stream, hdr);

	ASSERT_FALSE (error);
	ASSERT_EQ (received.data.version, nano::messages::telemetry_version::v1);
	ASSERT_EQ (received.data.database_backend, 0);
	ASSERT_EQ (received.data.confirmation_latency_ms, 0);
	ASSERT_EQ (received.data.bootstrap_status, 0);
	ASSERT_TRUE (received.validate_signature ());
	ASSERT_EQ (received.data.node_id, node_id.pub);
}

// New node (v2 payload) -> Old node: extra bytes are ignored during deserialization, signature validates via raw bytes
TEST (telemetry, forward_compat_new_to_old)
{
	nano::keypair node_id;
	nano::messages::telemetry_data data;
	data.node_id = node_id.pub;
	data.major_version = 20;
	data.minor_version = 1;
	data.patch_version = 5;
	data.pre_release_version = 2;
	data.maker = 1;
	data.database_backend = static_cast<uint8_t> (nano::messages::telemetry_database_backend::lmdb);
	data.confirmation_latency_ms = 250;
	data.bootstrap_status = static_cast<uint8_t> (nano::messages::telemetry_bootstrap_status::synced);
	data.timestamp = std::chrono::system_clock::time_point (100ms);
	data.active_difficulty = 0xffffffc000000000;

	// Create and sign a telemetry_ack with v2 data
	nano::messages::telemetry_ack ack{ nano::dev::network_params.network, data };
	ack.sign (node_id);
	ASSERT_TRUE (ack.validate_signature ());

	// Serialize the full message
	std::vector<uint8_t> message_bytes;
	{
		nano::vectorstream stream (message_bytes);
		ack.serialize (stream);
	}

	// Deserialize — simulates any node receiving this message
	auto header_size = nano::messages::message_header::size;
	bool error = false;
	nano::bufferstream header_stream (message_bytes.data (), header_size);
	nano::messages::message_header hdr (error, header_stream);
	ASSERT_FALSE (error);

	bool deser_error = false;
	nano::bufferstream payload_stream (message_bytes.data () + header_size, message_bytes.size () - header_size);
	nano::messages::telemetry_ack received (deser_error, payload_stream, hdr);

	ASSERT_FALSE (deser_error);
	// Signature validates against the raw payload bytes
	ASSERT_TRUE (received.validate_signature ());
	ASSERT_EQ (received.data.node_id, node_id.pub);
	ASSERT_EQ (received.data.database_backend, static_cast<uint8_t> (nano::messages::telemetry_database_backend::lmdb));
	ASSERT_EQ (received.data.confirmation_latency_ms, 250);
}

TEST (telemetry, no_peers)
{
	nano::test::system system (1);

	auto responses = system.nodes[0]->telemetry.get_all_telemetries ();
	ASSERT_TRUE (responses.empty ());
}

TEST (telemetry, basic)
{
	nano::test::system system;
	nano::node_flags node_flags;
	auto node_client = system.add_node (node_flags);
	auto node_server = system.add_node (node_flags);

	// Request telemetry metrics
	auto channel = node_client->network.find_node_id (node_server->get_node_id ());
	ASSERT_NE (nullptr, channel);

	std::optional<nano::messages::telemetry_data> telemetry_data;
	ASSERT_TIMELY (5s, telemetry_data = node_client->telemetry.get_telemetry (channel->get_remote_endpoint ()));
	ASSERT_EQ (node_server->get_node_id (), telemetry_data->node_id);

	// Check the metrics are correct
	ASSERT_TRUE (nano::test::compare_telemetry (*telemetry_data, *node_server));

	// Call again straight away
	auto telemetry_data_2 = node_client->telemetry.get_telemetry (channel->get_remote_endpoint ());
	ASSERT_TRUE (telemetry_data_2);

	// Call again straight away
	auto telemetry_data_3 = node_client->telemetry.get_telemetry (channel->get_remote_endpoint ());
	ASSERT_TRUE (telemetry_data_3);

	// we expect at least one consecutive repeat of telemetry
	ASSERT_TRUE (*telemetry_data == telemetry_data_2 || telemetry_data_2 == telemetry_data_3);

	// Wait the cache period and check cache is not used
	WAIT (3s);

	std::optional<nano::messages::telemetry_data> telemetry_data_4;
	ASSERT_TIMELY (5s, telemetry_data_4 = node_client->telemetry.get_telemetry (channel->get_remote_endpoint ()));
	ASSERT_NE (*telemetry_data, *telemetry_data_4);
}

TEST (telemetry, invalid_endpoint)
{
	nano::test::system system (2);

	auto node_client = system.nodes.front ();
	auto node_server = system.nodes.back ();

	node_client->telemetry.trigger ();

	// Give some time for nodes to exchange telemetry
	WAIT (1s);

	nano::endpoint endpoint = *nano::parse_endpoint ("::ffff:240.0.0.0:12345");
	ASSERT_FALSE (node_client->telemetry.get_telemetry (endpoint));
}

TEST (telemetry, disconnected)
{
	nano::test::system system;
	nano::node_flags node_flags;
	auto node_client = system.add_node (node_flags);
	auto node_server = system.add_node (node_flags);

	auto channel = node_client->network.find_node_id (node_server->get_node_id ());
	ASSERT_NE (nullptr, channel);

	// Ensure telemetry is available before disconnecting
	ASSERT_TIMELY (5s, node_client->telemetry.get_telemetry (channel->get_remote_endpoint ()));

	system.stop_node (*node_server);
	ASSERT_TRUE (channel);

	// Ensure telemetry from disconnected peer is removed
	ASSERT_TIMELY (5s, !node_client->telemetry.get_telemetry (channel->get_remote_endpoint ()));
}

TEST (telemetry, dos_tcp)
{
	// Confirm that telemetry_reqs are not processed
	nano::test::system system;
	nano::node_flags node_flags;
	auto node_client = system.add_node (node_flags);
	auto node_server = system.add_node (node_flags);

	auto channel = node_client->network.tcp_channels.find_node_id (node_server->get_node_id ());
	ASSERT_NE (nullptr, channel);

	nano::messages::telemetry_req message{ nano::dev::network_params.network };
	for (int i = 0; i < 10; ++i)
	{
		channel->send (message, nano::transport::traffic_type::test, [] (boost::system::error_code const & ec, size_t size_a) {
			ASSERT_FALSE (ec);
		});
	}

	// Should process telemetry_req messages
	ASSERT_TIMELY (5s, 1 < node_server->stats.count (nano::stat::type::message, nano::stat::detail::telemetry_req, nano::stat::dir::in));

	// But not respond to all of them (by default there are 2 broadcasts per second in dev mode)
	ASSERT_ALWAYS (1s, node_server->stats.count (nano::stat::type::message, nano::stat::detail::telemetry_ack, nano::stat::dir::out) < 7);
}

TEST (telemetry, disable_metrics)
{
	nano::test::system system;
	nano::node_flags node_flags;
	auto node_client = system.add_node (node_flags);
	node_flags.disable_providing_telemetry_metrics = true;
	auto node_server = system.add_node (node_flags);

	// Try and request metrics from a node which is turned off but a channel is not closed yet
	auto channel = node_client->network.find_node_id (node_server->get_node_id ());
	ASSERT_NE (nullptr, channel);

	node_client->telemetry.trigger ();

	ASSERT_NEVER (1s, node_client->telemetry.get_telemetry (channel->get_remote_endpoint ()));

	// It should still be able to receive metrics though
	auto channel1 = node_server->network.find_node_id (node_client->get_node_id ());
	ASSERT_NE (nullptr, channel1);

	std::optional<nano::messages::telemetry_data> telemetry_data;
	ASSERT_TIMELY (5s, telemetry_data = node_server->telemetry.get_telemetry (channel1->get_remote_endpoint ()));

	ASSERT_TRUE (nano::test::compare_telemetry (*telemetry_data, *node_client));
}

TEST (telemetry, max_possible_size)
{
	nano::test::system system;
	nano::node_flags node_flags;
	node_flags.disable_providing_telemetry_metrics = true;
	auto node_client = system.add_node (node_flags);
	auto node_server = system.add_node (node_flags);

	nano::messages::telemetry_data data;
	data.node_id = node_client->node_id.pub;

	nano::messages::telemetry_ack message{ nano::dev::network_params.network, data };
	message.sign (node_client->node_id);

	auto channel = node_client->network.tcp_channels.find_node_id (node_server->get_node_id ());
	ASSERT_NE (nullptr, channel);
	channel->send (message, nano::transport::traffic_type::test, [] (boost::system::error_code const & ec, size_t size_a) {
		ASSERT_FALSE (ec);
	});

	ASSERT_TIMELY_EQ (5s, 1, node_server->stats.count (nano::stat::type::message, nano::stat::detail::telemetry_ack, nano::stat::dir::in));
}

TEST (telemetry, maker_pruning)
{
	nano::test::system system;
	nano::node_flags node_flags;
	auto node_client = system.add_node (node_flags);
	node_flags.enable_pruning = true;
	nano::node_config config;
	config.enable_voting = false;
	auto node_server = system.add_node (config, node_flags);

	// Request telemetry metrics
	auto channel = node_client->network.find_node_id (node_server->get_node_id ());
	ASSERT_NE (nullptr, channel);

	std::optional<nano::messages::telemetry_data> telemetry_data;
	ASSERT_TIMELY (5s, telemetry_data = node_client->telemetry.get_telemetry (channel->get_remote_endpoint ()));
	ASSERT_EQ (node_server->get_node_id (), telemetry_data->node_id);

	// Ensure telemetry response indicates pruned node
	ASSERT_EQ (nano::messages::telemetry_maker::nf_pruned_node, static_cast<nano::messages::telemetry_maker> (telemetry_data->maker));
}

TEST (telemetry, invalid_signature)
{
	nano::test::system system;
	auto & node = *system.add_node ();

	auto telemetry = node.local_telemetry ();
	telemetry.block_count = 9999; // Change data so signature is no longer valid

	auto message = nano::messages::telemetry_ack{ nano::dev::network_params.network, telemetry };
	node.inbound (message, nano::test::fake_channel (node));

	ASSERT_TIMELY (5s, node.stats.count (nano::stat::type::telemetry, nano::stat::detail::invalid_signature) > 0);
	ASSERT_ALWAYS (1s, node.stats.count (nano::stat::type::telemetry, nano::stat::detail::process) == 0)
}

TEST (telemetry, mismatched_node_id)
{
	nano::test::system system;
	auto & node = *system.add_node ();

	auto telemetry = node.local_telemetry ();

	auto message = nano::messages::telemetry_ack{ nano::dev::network_params.network, telemetry };
	node.inbound (message, nano::test::fake_channel (node, /* node id */ { 123 }));

	ASSERT_TIMELY (5s, node.stats.count (nano::stat::type::telemetry, nano::stat::detail::node_id_mismatch) > 0);
	ASSERT_ALWAYS (1s, node.stats.count (nano::stat::type::telemetry, nano::stat::detail::process) == 0)
}

TEST (telemetry, ongoing_broadcasts)
{
	nano::test::system system;
	nano::node_flags node_flags;
	auto & node1 = *system.add_node (node_flags);
	auto & node2 = *system.add_node (node_flags);

	ASSERT_TIMELY (5s, node1.stats.count (nano::stat::type::telemetry, nano::stat::detail::process) >= 3);
	ASSERT_TIMELY (5s, node2.stats.count (nano::stat::type::telemetry, nano::stat::detail::process) >= 3)
}
