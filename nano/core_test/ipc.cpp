#include <nano/lib/blocks.hpp>
#include <nano/lib/ipc_client.hpp>
#include <nano/lib/tomlconfig.hpp>
#include <nano/node/ipc/ipc_access_config.hpp>
#include <nano/node/ipc/ipc_broker.hpp>
#include <nano/node/ipc/ipc_config.hpp>
#include <nano/node/ipc/ipc_server.hpp>
#include <nano/node/node_observers.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/rpc/rpc_server.hpp>
#include <nano/test_common/chains.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/property_tree/json_parser.hpp>

#include <chrono>
#include <memory>
#include <sstream>
#include <vector>

using namespace std::chrono_literals;

TEST (ipc, asynchronous)
{
	nano::test::system system (1);
	system.nodes[0]->config.ipc_config->transport_tcp.enabled = true;
	system.nodes[0]->config.ipc_config->transport_tcp.port = system.get_available_port ();
	nano::node_rpc_config node_rpc_config;
	nano::ipc::ipc_server ipc (*system.nodes[0], node_rpc_config);
	nano::ipc::ipc_client client (system.nodes[0]->io_ctx_shared);

	auto req (nano::ipc::prepare_request (nano::ipc::payload_encoding::json_v1, std::string (R"({"action": "block_count"})")));
	auto res (std::make_shared<std::vector<uint8_t>> ());
	std::atomic<bool> call_completed{ false };
	client.async_connect ("::1", ipc.listening_tcp_port ().value (), [&client, &req, &res, &call_completed] (nano::error err) {
		client.async_write (req, [&client, &req, &res, &call_completed] (nano::error err_a, size_t size_a) {
			ASSERT_NO_ERROR (static_cast<std::error_code> (err_a));
			ASSERT_EQ (size_a, req.size ());
			// Read length
			client.async_read (res, sizeof (uint32_t), [&client, &res, &call_completed] (nano::error err_read_a, size_t size_read_a) {
				ASSERT_NO_ERROR (static_cast<std::error_code> (err_read_a));
				ASSERT_EQ (size_read_a, sizeof (uint32_t));
				uint32_t payload_size_l = boost::endian::big_to_native (*reinterpret_cast<uint32_t *> (res->data ()));
				// Read json payload
				client.async_read (res, payload_size_l, [&res, &call_completed] (nano::error err_read_a, size_t size_read_a) {
					std::string payload (res->begin (), res->end ());
					std::stringstream ss;
					ss << payload;

					// Make sure the response is valid json
					boost::property_tree::ptree blocks;
					boost::property_tree::read_json (ss, blocks);
					ASSERT_EQ (blocks.get<int> ("count"), 1);
					call_completed = true;
				});
			});
		});
	});
	ASSERT_TIMELY (5s, call_completed);
	ipc.stop ();
}

TEST (ipc, synchronous)
{
	nano::test::system system (1);
	system.nodes[0]->config.ipc_config->transport_tcp.enabled = true;
	system.nodes[0]->config.ipc_config->transport_tcp.port = system.get_available_port ();
	nano::node_rpc_config node_rpc_config;
	nano::ipc::ipc_server ipc (*system.nodes[0], node_rpc_config);
	nano::ipc::ipc_client client (system.nodes[0]->io_ctx_shared);

	// Start blocking IPC client in a separate thread
	std::atomic<bool> call_completed{ false };
	std::thread client_thread ([&] () {
		client.connect ("::1", ipc.listening_tcp_port ().value ());
		std::string response (nano::ipc::request (nano::ipc::payload_encoding::json_v1, client, std::string (R"({"action": "block_count"})")));
		std::stringstream ss;
		ss << response;
		// Make sure the response is valid json
		boost::property_tree::ptree blocks;
		boost::property_tree::read_json (ss, blocks);
		ASSERT_EQ (blocks.get<int> ("count"), 1);

		call_completed = true;
	});
	client_thread.detach ();

	ASSERT_TIMELY (5s, call_completed);
	ipc.stop ();
}

TEST (ipc, permissions_default_user)
{
	// Test empty/nonexistent access config. The default user still exists with default permissions.
	std::stringstream ss;
	ss << R"toml(
	)toml";

	nano::tomlconfig toml;
	toml.read (ss);

	nano::ipc::access access;
	access.deserialize_toml (toml);
	ASSERT_TRUE (access.has_access ("", nano::ipc::access_permission::api_account_weight));
}

TEST (ipc, permissions_deny_default)
{
	// All users have api_account_weight permissions by default. This removes the permission for a specific user.
	std::stringstream ss;
	ss << R"toml(
	[[user]]
	id = "user1"
	deny = "api_account_weight"
	)toml";

	nano::tomlconfig toml;
	toml.read (ss);

	nano::ipc::access access;
	access.deserialize_toml (toml);
	ASSERT_FALSE (access.has_access ("user1", nano::ipc::access_permission::api_account_weight));
}

TEST (ipc, permissions_groups)
{
	// Make sure role permissions are adopted by user
	std::stringstream ss;
	ss << R"toml(
	[[role]]
	id = "mywalletadmin"
	allow = "wallet_read, wallet_write"

	[[user]]
	id = "user1"
	roles = "mywalletadmin"
	deny = "api_account_weight"
	)toml";

	nano::tomlconfig toml;
	toml.read (ss);

	nano::ipc::access access;
	access.deserialize_toml (toml);
	ASSERT_FALSE (access.has_access ("user1", nano::ipc::access_permission::api_account_weight));
	ASSERT_TRUE (access.has_access_to_all ("user1", { nano::ipc::access_permission::wallet_read, nano::ipc::access_permission::wallet_write }));
}

TEST (ipc, permissions_oneof)
{
	// Test one of two permissions
	std::stringstream ss;
	ss << R"toml(
	[[user]]
	id = "user1"
	allow = "api_account_weight"
	[[user]]
	id = "user2"
	allow = "api_account_weight, account_query"
	[[user]]
	id = "user3"
	deny = "api_account_weight, account_query"
	)toml";

	nano::tomlconfig toml;
	toml.read (ss);

	nano::ipc::access access;
	access.deserialize_toml (toml);
	ASSERT_TRUE (access.has_access ("user1", nano::ipc::access_permission::api_account_weight));
	ASSERT_TRUE (access.has_access ("user2", nano::ipc::access_permission::api_account_weight));
	ASSERT_FALSE (access.has_access ("user3", nano::ipc::access_permission::api_account_weight));
	ASSERT_TRUE (access.has_access_to_oneof ("user1", { nano::ipc::access_permission::account_query, nano::ipc::access_permission::api_account_weight }));
	ASSERT_TRUE (access.has_access_to_oneof ("user2", { nano::ipc::access_permission::account_query, nano::ipc::access_permission::api_account_weight }));
	ASSERT_FALSE (access.has_access_to_oneof ("user3", { nano::ipc::access_permission::account_query, nano::ipc::access_permission::api_account_weight }));
}

TEST (ipc, permissions_default_user_order)
{
	// If changing the default user, it must come first
	std::stringstream ss;
	ss << R"toml(
	[[user]]
	id = "user1"
	[[user]]
	id = ""
	)toml";

	nano::tomlconfig toml;
	toml.read (ss);

	nano::ipc::access access;
	ASSERT_TRUE (access.deserialize_toml (toml));
}

TEST (ipc, invalid_endpoint)
{
	nano::test::system system (1);
	system.nodes[0]->config.ipc_config->transport_tcp.enabled = true;
	system.nodes[0]->config.ipc_config->transport_tcp.port = system.get_available_port ();
	nano::node_rpc_config node_rpc_config;
	nano::ipc::ipc_client client (system.nodes[0]->io_ctx_shared);

	std::atomic<bool> call_completed{ false };
	client.async_connect ("::-1", 24077, [&client, &call_completed] (nano::error err) {
		ASSERT_IS_ERROR (static_cast<std::error_code> (err));
		call_completed = true;
	});
	ASSERT_TIMELY (5s, call_completed);
}

namespace
{
class confirmation_subscriber final : public nano::ipc::subscriber
{
public:
	void async_send_message (uint8_t const * data, std::size_t, std::function<void (nano::error const &)>) override
	{
		auto const * confirmation = nanoapi::GetEnvelope (data)->message_as_EventConfirmation ();
		ASSERT_NE (nullptr, confirmation);
		blocks.push_back (confirmation->block_type ());
		hashes.push_back (confirmation->hash () ? confirmation->hash ()->str () : std::string{});
	}

	uint64_t get_id () const override
	{
		return 1;
	}
	std::string get_service_name () const override
	{
		return {};
	}
	void set_service_name (std::string const &) override
	{
	}
	nano::ipc::payload_encoding get_active_encoding () const override
	{
		return nano::ipc::payload_encoding::flatbuffers;
	}

	std::vector<nanoapi::Block> blocks;
	std::vector<std::string> hashes;
};
}

TEST (ipc, filtered_confirmation_without_block)
{
	nano::test::system system (1);
	auto & node = *system.nodes.front ();
	auto const send = nano::test::setup_chain (system, node, 1).front ();
	auto broker = std::make_shared<nano::ipc::broker> (node);
	broker->start ();
	auto subscribe = [&] (nano::account const & account, bool include_block) {
		auto client = std::make_shared<confirmation_subscriber> ();
		auto topic = std::make_shared<nanoapi::TopicConfirmationT> ();
		topic->options = std::make_unique<nanoapi::TopicConfirmationOptionsT> ();
		topic->options->accounts.push_back (account.to_account ());
		topic->options->include_block = include_block;
		broker->subscribe (client, topic);
		return client;
	};

	auto sender = subscribe (nano::dev::genesis_key.pub, false);
	auto destination = subscribe (send->destination (), false);
	auto unrelated = subscribe (nano::keypair{}.pub, false);
	auto full = subscribe (send->destination (), true);

	node.observers.block_confirmed.notify ({ .status = { .winner = send }, .type = nano::confirmation_type::inactive_confirmation_height, .account = nano::dev::genesis_key.pub, .amount = 1, .is_state_send = true });
	EXPECT_EQ ((std::vector{ nanoapi::Block_NONE }), sender->blocks);
	EXPECT_EQ ((std::vector{ nanoapi::Block_NONE }), destination->blocks);
	EXPECT_TRUE (unrelated->blocks.empty ());
	EXPECT_EQ ((std::vector{ nanoapi::Block_BlockState }), full->blocks);
}

TEST (ipc, confirmation_hash_without_block)
{
	nano::test::system system (1);
	auto & node = *system.nodes.front ();
	auto const send = nano::test::setup_chain (system, node, 1).front ();
	auto broker = std::make_shared<nano::ipc::broker> (node);
	broker->start ();

	auto client = std::make_shared<confirmation_subscriber> ();
	auto topic = std::make_shared<nanoapi::TopicConfirmationT> ();
	topic->options = std::make_unique<nanoapi::TopicConfirmationOptionsT> ();
	topic->options->include_block = false;
	broker->subscribe (client, topic);

	node.observers.block_confirmed.notify ({ .status = { .winner = send }, .type = nano::confirmation_type::inactive_confirmation_height, .account = nano::dev::genesis_key.pub, .amount = 1, .is_state_send = true });
	EXPECT_EQ ((std::vector{ send->hash ().to_string () }), client->hashes);
}
