#include <nano/lib/blocks.hpp>
#include <nano/lib/stream.hpp>
#include <nano/lib/work_version.hpp>
#include <nano/messages/messages.hpp>
#include <nano/node/endpoint.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/property_tree/json_parser.hpp>

#include <thread>

#include <crypto/ed25519-donna/ed25519.h>

TEST (ed25519, signing)
{
	nano::raw_key prv (0);
	auto pub (nano::pub_key (prv));
	nano::uint256_union message (0);
	nano::signature signature;
	ed25519_sign (message.bytes.data (), sizeof (message.bytes), prv.bytes.data (), pub.bytes.data (), signature.bytes.data ());
	auto valid1 (ed25519_sign_open (message.bytes.data (), sizeof (message.bytes), pub.bytes.data (), signature.bytes.data ()));
	ASSERT_EQ (0, valid1);
	signature.bytes[32] ^= 0x1;
	auto valid2 (ed25519_sign_open (message.bytes.data (), sizeof (message.bytes), pub.bytes.data (), signature.bytes.data ()));
	ASSERT_NE (0, valid2);
}

TEST (transaction_block, empty)
{
	nano::keypair key1;
	nano::block_builder builder;
	auto block = builder
				 .send ()
				 .previous (0)
				 .destination (1)
				 .balance (13)
				 .sign (key1.prv, key1.pub)
				 .work (2)
				 .build ();
	auto hash (block.hash ());
	ASSERT_FALSE (nano::validate_message (key1.pub, hash, block.block_signature ()));
	auto send = block.as_send ().value ();
	send.signature.bytes[32] ^= 0x1;
	auto modified = nano::raw_block{ send };
	ASSERT_TRUE (nano::validate_message (key1.pub, hash, modified.block_signature ()));
}

TEST (block, send_serialize)
{
	nano::block_builder builder;
	auto block1 = builder
				  .send ()
				  .previous (0)
				  .destination (1)
				  .balance (2)
				  .sign (nano::keypair ().prv, 4)
				  .work (5)
				  .build ();
	std::vector<uint8_t> bytes;
	{
		nano::vectorstream stream1 (bytes);
		block1.serialize (stream1);
	}
	auto data (bytes.data ());
	auto size (bytes.size ());
	ASSERT_NE (nullptr, data);
	ASSERT_NE (0, size);
	nano::bufferstream stream2 (data, size);
	auto block2 = nano::deserialize_raw_block (stream2, nano::block_type::send);
	ASSERT_EQ (block1, block2);
}

TEST (block, send_serialize_json)
{
	nano::block_builder builder;
	auto block1 = builder
				  .send ()
				  .previous (0)
				  .destination (1)
				  .balance (2)
				  .sign (nano::keypair ().prv, 4)
				  .work (5)
				  .build ();
	std::string string1;
	block1.serialize_json (string1);
	ASSERT_NE (0, string1.size ());
	boost::property_tree::ptree tree1;
	std::stringstream istream (string1);
	boost::property_tree::read_json (istream, tree1);
	auto block2 = nano::deserialize_raw_block_json (tree1);
	ASSERT_EQ (block1, block2);
}

TEST (block, receive_serialize)
{
	nano::block_builder builder;
	auto block1 = builder
				  .receive ()
				  .previous (0)
				  .source (1)
				  .sign (nano::keypair ().prv, 3)
				  .work (4)
				  .build ();
	nano::keypair key1;
	std::vector<uint8_t> bytes;
	{
		nano::vectorstream stream1 (bytes);
		block1.serialize (stream1);
	}
	nano::bufferstream stream2 (bytes.data (), bytes.size ());
	auto block2 = nano::deserialize_raw_block (stream2, nano::block_type::receive);
	ASSERT_EQ (block1, block2);
}

TEST (block, receive_serialize_json)
{
	nano::block_builder builder;
	auto block1 = builder
				  .receive ()
				  .previous (0)
				  .source (1)
				  .sign (nano::keypair ().prv, 3)
				  .work (4)
				  .build ();
	std::string string1;
	block1.serialize_json (string1);
	ASSERT_NE (0, string1.size ());
	boost::property_tree::ptree tree1;
	std::stringstream istream (string1);
	boost::property_tree::read_json (istream, tree1);
	auto block2 = nano::deserialize_raw_block_json (tree1);
	ASSERT_EQ (block1, block2);
}

TEST (block, open_serialize_json)
{
	nano::block_builder builder;
	auto block1 = builder
				  .open ()
				  .source (0)
				  .representative (1)
				  .account (0)
				  .sign (nano::keypair ().prv, 0)
				  .work (0)
				  .build ();
	std::string string1;
	block1.serialize_json (string1);
	ASSERT_NE (0, string1.size ());
	boost::property_tree::ptree tree1;
	std::stringstream istream (string1);
	boost::property_tree::read_json (istream, tree1);
	auto block2 = nano::deserialize_raw_block_json (tree1);
	ASSERT_EQ (block1, block2);
}

TEST (block, change_serialize_json)
{
	nano::block_builder builder;
	auto block1 = builder
				  .change ()
				  .previous (0)
				  .representative (1)
				  .sign (nano::keypair ().prv, 3)
				  .work (4)
				  .build ();
	std::string string1;
	block1.serialize_json (string1);
	ASSERT_NE (0, string1.size ());
	boost::property_tree::ptree tree1;
	std::stringstream istream (string1);
	boost::property_tree::read_json (istream, tree1);
	auto block2 = nano::deserialize_raw_block_json (tree1);
	ASSERT_EQ (block1, block2);
}

TEST (send_block, deserialize)
{
	nano::block_builder builder;
	auto block1 = builder
				  .send ()
				  .previous (0)
				  .destination (1)
				  .balance (2)
				  .sign (nano::keypair ().prv, 4)
				  .work (5)
				  .build ();
	ASSERT_EQ (block1.hash (), block1.hash ());
	std::vector<uint8_t> bytes;
	{
		nano::vectorstream stream1 (bytes);
		block1.serialize (stream1);
	}
	ASSERT_EQ (nano::raw_send_block::size, bytes.size ());
	nano::bufferstream stream2 (bytes.data (), bytes.size ());
	auto block2 = nano::deserialize_raw_block (stream2, nano::block_type::send);
	ASSERT_EQ (block1, block2);
}

TEST (receive_block, deserialize)
{
	nano::block_builder builder;
	auto block1 = builder
				  .receive ()
				  .previous (0)
				  .source (1)
				  .sign (nano::keypair ().prv, 3)
				  .work (4)
				  .build ();
	ASSERT_EQ (block1.hash (), block1.hash ());
	auto recv = block1.as_receive ().value ();
	recv.hashables.previous = 2;
	recv.hashables.source = 4;
	block1 = nano::raw_block{ recv };
	std::vector<uint8_t> bytes;
	{
		nano::vectorstream stream1 (bytes);
		block1.serialize (stream1);
	}
	ASSERT_EQ (nano::raw_receive_block::size, bytes.size ());
	nano::bufferstream stream2 (bytes.data (), bytes.size ());
	auto block2 = nano::deserialize_raw_block (stream2, nano::block_type::receive);
	ASSERT_EQ (block1, block2);
}

TEST (open_block, deserialize)
{
	nano::block_builder builder;
	auto block1 = builder
				  .open ()
				  .source (0)
				  .representative (1)
				  .account (0)
				  .sign (nano::keypair ().prv, 0)
				  .work (0)
				  .build ();
	ASSERT_EQ (block1.hash (), block1.hash ());
	std::vector<uint8_t> bytes;
	{
		nano::vectorstream stream (bytes);
		block1.serialize (stream);
	}
	ASSERT_EQ (nano::raw_open_block::size, bytes.size ());
	nano::bufferstream stream (bytes.data (), bytes.size ());
	auto block2 = nano::deserialize_raw_block (stream, nano::block_type::open);
	ASSERT_EQ (block1, block2);
}

TEST (change_block, deserialize)
{
	nano::block_builder builder;
	auto block1 = builder
				  .change ()
				  .previous (1)
				  .representative (2)
				  .sign (nano::keypair ().prv, 4)
				  .work (5)
				  .build ();
	ASSERT_EQ (block1.hash (), block1.hash ());
	std::vector<uint8_t> bytes;
	{
		nano::vectorstream stream1 (bytes);
		block1.serialize (stream1);
	}
	ASSERT_EQ (nano::raw_change_block::size, bytes.size ());
	auto data (bytes.data ());
	auto size (bytes.size ());
	ASSERT_NE (nullptr, data);
	ASSERT_NE (0, size);
	nano::bufferstream stream2 (data, size);
	auto block2 = nano::deserialize_raw_block (stream2, nano::block_type::change);
	ASSERT_EQ (block1, block2);
}

TEST (frontier_req, serialization)
{
	nano::messages::frontier_req request1{ nano::dev::network_params.network };
	request1.start = 1;
	request1.age = 2;
	request1.count = 3;
	std::vector<uint8_t> bytes;
	{
		nano::vectorstream stream (bytes);
		request1.serialize (stream);
	}
	auto error (false);
	nano::bufferstream stream (bytes.data (), bytes.size ());
	nano::messages::message_header header (error, stream);
	ASSERT_FALSE (error);
	nano::messages::frontier_req request2 (error, stream, header);
	ASSERT_FALSE (error);
	ASSERT_EQ (request1, request2);
}

TEST (block, publish_req_serialization)
{
	nano::keypair key1;
	nano::keypair key2;
	nano::block_builder builder;
	auto block = builder
				 .send ()
				 .previous (0)
				 .destination (key2.pub)
				 .balance (200)
				 .sign (nano::keypair ().prv, 2)
				 .work (3)
				 .build ();
	nano::messages::publish req{ nano::dev::network_params.network, block };
	std::vector<uint8_t> bytes;
	{
		nano::vectorstream stream (bytes);
		req.serialize (stream);
	}
	auto error (false);
	nano::bufferstream stream2 (bytes.data (), bytes.size ());
	nano::messages::message_header header (error, stream2);
	ASSERT_FALSE (error);
	nano::messages::publish req2 (error, stream2, header);
	ASSERT_FALSE (error);
	ASSERT_EQ (req, req2);
	ASSERT_EQ (req.block, req2.block);
}

TEST (block, difficulty)
{
	nano::block_builder builder;
	auto block = builder
				 .send ()
				 .previous (0)
				 .destination (1)
				 .balance (2)
				 .sign (nano::keypair ().prv, 4)
				 .work (5)
				 .build ();
	auto difficulty = nano::dev::network_params.work.difficulty (block.work_version (), block.root (), block.block_work ());
	ASSERT_EQ (difficulty, nano::dev::network_params.work.difficulty (block.work_version (), block.root (), block.block_work ()));
}

TEST (state_block, serialization)
{
	nano::keypair key1;
	nano::keypair key2;
	nano::state_block_builder builder;
	auto block1 = builder
				  .account (key1.pub)
				  .previous (1)
				  .representative (key2.pub)
				  .balance (2)
				  .link (4)
				  .sign (key1.prv, key1.pub)
				  .work (5)
				  .build ();
	auto state1 = block1.as_state ().value ();
	ASSERT_EQ (key1.pub, state1.hashables.account);
	ASSERT_EQ (nano::block_hash (1), block1.previous ());
	ASSERT_EQ (key2.pub, state1.hashables.representative);
	ASSERT_EQ (nano::amount (2), state1.hashables.balance);
	ASSERT_EQ (nano::uint256_union (4), state1.hashables.link);
	std::vector<uint8_t> bytes;
	{
		nano::vectorstream stream (bytes);
		block1.serialize (stream);
	}
	ASSERT_EQ (0x5, bytes[215]); // Ensure work is serialized big-endian
	ASSERT_EQ (nano::raw_state_block::size, bytes.size ());
	{
		nano::bufferstream stream (bytes.data (), bytes.size ());
		auto block2 = nano::deserialize_raw_block (stream, nano::block_type::state);
		ASSERT_EQ (block1, block2);
	}
	// Test that re-deserialization from the same bytes produces the same result
	{
		nano::bufferstream stream2 (bytes.data (), bytes.size ());
		auto block2b = nano::deserialize_raw_block (stream2, nano::block_type::state);
		ASSERT_EQ (block1, block2b);
	}
	std::string json;
	block1.serialize_json (json);
	std::stringstream body (json);
	boost::property_tree::ptree tree;
	boost::property_tree::read_json (body, tree);
	{
		auto block3 = nano::deserialize_raw_block_json (tree);
		ASSERT_EQ (block1, block3);
	}
	// Test that re-deserialization from the same JSON produces the same result
	{
		auto block3b = nano::deserialize_raw_block_json (tree);
		ASSERT_EQ (block1, block3b);
	}
}

TEST (state_block, hashing)
{
	nano::keypair key;
	nano::state_block_builder builder;
	auto block = builder
				 .account (key.pub)
				 .previous (0)
				 .representative (key.pub)
				 .balance (0)
				 .link (0)
				 .sign (key.prv, key.pub)
				 .work (0)
				 .build ();
	auto hash (block.hash ());
	ASSERT_EQ (hash, block.hash ()); // check cache works

	// Helper to modify a state hashable field and rebuild
	auto modify_and_check = [&] (auto modifier) {
		auto state = block.as_state ().value ();
		modifier (state);
		block = nano::raw_block{ state };
	};

	modify_and_check ([&] (nano::raw_state_block & s) { s.hashables.account.bytes[0] ^= 0x1; });
	ASSERT_NE (hash, block.hash ());
	modify_and_check ([&] (nano::raw_state_block & s) { s.hashables.account.bytes[0] ^= 0x1; });
	ASSERT_EQ (hash, block.hash ());

	modify_and_check ([&] (nano::raw_state_block & s) { s.hashables.previous.bytes[0] ^= 0x1; });
	ASSERT_NE (hash, block.hash ());
	modify_and_check ([&] (nano::raw_state_block & s) { s.hashables.previous.bytes[0] ^= 0x1; });
	ASSERT_EQ (hash, block.hash ());

	modify_and_check ([&] (nano::raw_state_block & s) { s.hashables.representative.bytes[0] ^= 0x1; });
	ASSERT_NE (hash, block.hash ());
	modify_and_check ([&] (nano::raw_state_block & s) { s.hashables.representative.bytes[0] ^= 0x1; });
	ASSERT_EQ (hash, block.hash ());

	modify_and_check ([&] (nano::raw_state_block & s) { s.hashables.balance.bytes[0] ^= 0x1; });
	ASSERT_NE (hash, block.hash ());
	modify_and_check ([&] (nano::raw_state_block & s) { s.hashables.balance.bytes[0] ^= 0x1; });
	ASSERT_EQ (hash, block.hash ());

	modify_and_check ([&] (nano::raw_state_block & s) { s.hashables.link.bytes[0] ^= 0x1; });
	ASSERT_NE (hash, block.hash ());
	modify_and_check ([&] (nano::raw_state_block & s) { s.hashables.link.bytes[0] ^= 0x1; });
	ASSERT_EQ (hash, block.hash ());
}

TEST (blocks, work_version)
{
	ASSERT_EQ (nano::work_version::work_1, nano::raw_send_block{}.work_version ());
	ASSERT_EQ (nano::work_version::work_1, nano::raw_receive_block{}.work_version ());
	ASSERT_EQ (nano::work_version::work_1, nano::raw_change_block{}.work_version ());
	ASSERT_EQ (nano::work_version::work_1, nano::raw_open_block{}.work_version ());
	ASSERT_EQ (nano::work_version::work_1, nano::raw_state_block{}.work_version ());
}

TEST (block_builder, from)
{
	std::error_code ec;
	nano::block_builder builder;
	auto block = builder
				 .state ()
				 .account_address ("xrb_15nhh1kzw3x8ohez6s75wy3jr6dqgq65oaede1fzk5hqxk4j8ehz7iqtb3to")
				 .previous_hex ("FEFBCE274E75148AB31FF63EFB3082EF1126BF72BF3FA9C76A97FD5A9F0EBEC5")
				 .balance_dec ("2251569974100400000000000000000000")
				 .representative_address ("xrb_1stofnrxuz3cai7ze75o174bpm7scwj9jn3nxsn8ntzg784jf1gzn1jjdkou")
				 .link_hex ("E16DD58C1EFA8B521545B0A74375AA994D9FC43828A4266D75ECF57F07A7EE86")
				 .build (ec);
	ASSERT_EQ (block.hash ().to_string (), "2D243F8F92CDD0AD94A1D456A6B15F3BE7A6FCBD98D4C5831D06D15C818CD81F");

	auto block2 = builder.state ().from (block).build (ec);
	ASSERT_EQ (block2.hash ().to_string (), "2D243F8F92CDD0AD94A1D456A6B15F3BE7A6FCBD98D4C5831D06D15C818CD81F");

	auto block3 = builder.state ().from (block).sign_zero ().work (0).build (ec);
	ASSERT_EQ (block3.hash ().to_string (), "2D243F8F92CDD0AD94A1D456A6B15F3BE7A6FCBD98D4C5831D06D15C818CD81F");
}

TEST (block_builder, zeroed_state_block)
{
	nano::block_builder builder;
	nano::keypair key;
	nano::state_block_builder state_builder;
	// Make sure manually- and builder constructed all-zero blocks have equal hashes, and check signature.
	auto zero_block_manual = state_builder
							 .account (0)
							 .previous (0)
							 .representative (0)
							 .balance (0)
							 .link (0)
							 .sign (key.prv, key.pub)
							 .work (0)
							 .build ();
	auto zero_block_build = builder.state ().zero ().sign (key.prv, key.pub).build ();
	ASSERT_EQ (zero_block_manual.hash (), zero_block_build.hash ());
	ASSERT_FALSE (nano::validate_message (key.pub, zero_block_build.hash (), zero_block_build.block_signature ()));
}

TEST (block_builder, state)
{
	// Test against a random hash from the live network
	std::error_code ec;
	nano::block_builder builder;
	auto block = builder
				 .state ()
				 .account_address ("xrb_15nhh1kzw3x8ohez6s75wy3jr6dqgq65oaede1fzk5hqxk4j8ehz7iqtb3to")
				 .previous_hex ("FEFBCE274E75148AB31FF63EFB3082EF1126BF72BF3FA9C76A97FD5A9F0EBEC5")
				 .balance_dec ("2251569974100400000000000000000000")
				 .representative_address ("xrb_1stofnrxuz3cai7ze75o174bpm7scwj9jn3nxsn8ntzg784jf1gzn1jjdkou")
				 .link_hex ("E16DD58C1EFA8B521545B0A74375AA994D9FC43828A4266D75ECF57F07A7EE86")
				 .build (ec);
	ASSERT_EQ (block.hash ().to_string (), "2D243F8F92CDD0AD94A1D456A6B15F3BE7A6FCBD98D4C5831D06D15C818CD81F");
	ASSERT_FALSE (block.source_field ());
	ASSERT_FALSE (block.destination_field ());
	ASSERT_EQ (block.link_field ().value ().to_string (), "E16DD58C1EFA8B521545B0A74375AA994D9FC43828A4266D75ECF57F07A7EE86");
}

TEST (block_builder, state_missing_rep)
{
	// Test against a random hash from the live network
	std::error_code ec;
	nano::block_builder builder;
	auto block = builder
				 .state ()
				 .account_address ("xrb_15nhh1kzw3x8ohez6s75wy3jr6dqgq65oaede1fzk5hqxk4j8ehz7iqtb3to")
				 .previous_hex ("FEFBCE274E75148AB31FF63EFB3082EF1126BF72BF3FA9C76A97FD5A9F0EBEC5")
				 .balance_dec ("2251569974100400000000000000000000")
				 .link_hex ("E16DD58C1EFA8B521545B0A74375AA994D9FC43828A4266D75ECF57F07A7EE86")
				 .sign_zero ()
				 .work (0)
				 .build (ec);
	ASSERT_EQ (ec, nano::error_common::missing_representative);
}

TEST (block_builder, state_equality)
{
	std::error_code ec;
	nano::block_builder builder;

	// With constructor
	nano::keypair key1, key2;
	nano::state_block_builder state_builder;
	auto block1 = state_builder
				  .account (key1.pub)
				  .previous (1)
				  .representative (key2.pub)
				  .balance (2)
				  .link (4)
				  .sign (key1.prv, key1.pub)
				  .work (5)
				  .build ();

	// With builder
	auto block2 = builder
				  .state ()
				  .account (key1.pub)
				  .previous (1)
				  .representative (key2.pub)
				  .balance (2)
				  .link (4)
				  .sign (key1.prv, key1.pub)
				  .work (5)
				  .build (ec);

	ASSERT_NO_ERROR (ec);
	ASSERT_EQ (block1.hash (), block2.hash ());
	ASSERT_EQ (block1.block_work (), block2.block_work ());
}

TEST (block_builder, state_errors)
{
	std::error_code ec;
	nano::block_builder builder;

	// Ensure the proper error is generated
	builder.state ().account_hex ("xrb_bad").build (ec);
	ASSERT_EQ (ec, nano::error_common::bad_account_number);

	builder.state ().zero ().account_address ("xrb_1111111111111111111111111111111111111111111111111111hifc8npp").build (ec);
	ASSERT_NO_ERROR (ec);
}

TEST (block_builder, open)
{
	// Test built block's hash against the Genesis open block from the live network
	std::error_code ec;
	nano::block_builder builder;
	auto block = builder
				 .open ()
				 .account_address ("xrb_3t6k35gi95xu6tergt6p69ck76ogmitsa8mnijtpxm9fkcm736xtoncuohr3")
				 .representative_address ("xrb_3t6k35gi95xu6tergt6p69ck76ogmitsa8mnijtpxm9fkcm736xtoncuohr3")
				 .source_hex ("E89208DD038FBB269987689621D52292AE9C35941A7484756ECCED92A65093BA")
				 .build (ec);
	ASSERT_EQ (block.hash ().to_string (), "991CF190094C00F0B68E2E5F75F6BEE95A2E0BD93CEAA4A6734DB9F19B728948");
	ASSERT_EQ (block.source_field ().value ().to_string (), "E89208DD038FBB269987689621D52292AE9C35941A7484756ECCED92A65093BA");
	ASSERT_FALSE (block.destination_field ());
	ASSERT_FALSE (block.link_field ());
}

TEST (block_builder, open_equality)
{
	std::error_code ec;
	nano::block_builder builder;

	// With builder (reference)
	nano::keypair key1, key2;
	auto block1 = builder
				  .open ()
				  .source (1)
				  .account (key2.pub)
				  .representative (key1.pub)
				  .sign (key1.prv, key1.pub)
				  .work (5)
				  .build ();

	// With builder (using ec)
	nano::block_builder builder2;
	auto block2 = builder2
				  .open ()
				  .source (1)
				  .account (key2.pub)
				  .representative (key1.pub)
				  .sign (key1.prv, key1.pub)
				  .work (5)
				  .build (ec);

	ASSERT_NO_ERROR (ec);
	ASSERT_EQ (block1.hash (), block2.hash ());
	ASSERT_EQ (block1.block_work (), block2.block_work ());
}

TEST (block_builder, change)
{
	std::error_code ec;
	nano::block_builder builder;
	auto block = builder
				 .change ()
				 .representative_address ("xrb_3rropjiqfxpmrrkooej4qtmm1pueu36f9ghinpho4esfdor8785a455d16nf")
				 .previous_hex ("088EE46429CA936F76C4EAA20B97F6D33E5D872971433EE0C1311BCB98764456")
				 .build (ec);
	ASSERT_EQ (block.hash ().to_string (), "13552AC3928E93B5C6C215F61879358E248D4A5246B8B3D1EEC5A566EDCEE077");
	ASSERT_FALSE (block.source_field ());
	ASSERT_FALSE (block.destination_field ());
	ASSERT_FALSE (block.link_field ());
}

TEST (block_builder, change_equality)
{
	std::error_code ec;
	nano::block_builder builder;

	// With builder (reference)
	nano::keypair key1, key2;
	auto block1 = builder
				  .change ()
				  .previous (1)
				  .representative (key1.pub)
				  .sign (key1.prv, key1.pub)
				  .work (5)
				  .build ();

	// With builder (using ec)
	nano::block_builder builder2;
	auto block2 = builder2
				  .change ()
				  .previous (1)
				  .representative (key1.pub)
				  .sign (key1.prv, key1.pub)
				  .work (5)
				  .build (ec);

	ASSERT_NO_ERROR (ec);
	ASSERT_EQ (block1.hash (), block2.hash ());
	ASSERT_EQ (block1.block_work (), block2.block_work ());
}

TEST (block_builder, send)
{
	std::error_code ec;
	nano::block_builder builder;
	auto block = builder
				 .send ()
				 .destination_address ("xrb_1gys8r4crpxhp94n4uho5cshaho81na6454qni5gu9n53gksoyy1wcd4udyb")
				 .previous_hex ("F685856D73A488894F7F3A62BC3A88E17E985F9969629FF3FDD4A0D4FD823F24")
				 .balance_hex ("00F035A9C7D818E7C34148C524FFFFEE")
				 .build (ec);
	ASSERT_EQ (block.hash ().to_string (), "4560E7B1F3735D082700CFC2852F5D1F378F7418FD24CEF1AD45AB69316F15CD");
	ASSERT_FALSE (block.source_field ());
	ASSERT_EQ (block.destination_field ().value ().to_account (), "nano_1gys8r4crpxhp94n4uho5cshaho81na6454qni5gu9n53gksoyy1wcd4udyb");
	ASSERT_FALSE (block.link_field ());
}

TEST (block_builder, send_equality)
{
	std::error_code ec;
	nano::block_builder builder;

	// With builder (reference)
	nano::keypair key1, key2;
	auto block1 = builder
				  .send ()
				  .previous (1)
				  .destination (key1.pub)
				  .balance (2)
				  .sign (key1.prv, key1.pub)
				  .work (5)
				  .build ();

	// With builder (using ec)
	nano::block_builder builder2;
	auto block2 = builder2
				  .send ()
				  .previous (1)
				  .destination (key1.pub)
				  .balance (2)
				  .sign (key1.prv, key1.pub)
				  .work (5)
				  .build (ec);

	ASSERT_NO_ERROR (ec);
	ASSERT_EQ (block1.hash (), block2.hash ());
	ASSERT_EQ (block1.block_work (), block2.block_work ());
}

TEST (block_builder, receive_equality)
{
	std::error_code ec;
	nano::block_builder builder;

	// With builder (reference)
	nano::keypair key1;
	auto block1 = builder
				  .receive ()
				  .previous (1)
				  .source (2)
				  .sign (key1.prv, key1.pub)
				  .work (5)
				  .build ();

	// With builder (using ec)
	nano::block_builder builder2;
	auto block2 = builder2
				  .receive ()
				  .previous (1)
				  .source (2)
				  .sign (key1.prv, key1.pub)
				  .work (5)
				  .build (ec);

	ASSERT_NO_ERROR (ec);
	ASSERT_EQ (block1.hash (), block2.hash ());
	ASSERT_EQ (block1.block_work (), block2.block_work ());
}

TEST (block_builder, receive)
{
	std::error_code ec;
	nano::block_builder builder;
	auto block = builder
				 .receive ()
				 .previous_hex ("59660153194CAC5DAC08509D87970BF86F6AEA943025E2A7ED7460930594950E")
				 .source_hex ("7B2B0A29C1B235FDF9B4DEF2984BB3573BD1A52D28246396FBB3E4C5FE662135")
				 .build (ec);
	ASSERT_EQ (block.hash ().to_string (), "6C004BF911D9CF2ED75CF6EC45E795122AD5D093FF5A83EDFBA43EC4A3EDC722");
	ASSERT_EQ (block.source_field ().value ().to_string (), "7B2B0A29C1B235FDF9B4DEF2984BB3573BD1A52D28246396FBB3E4C5FE662135");
	ASSERT_FALSE (block.destination_field ());
	ASSERT_FALSE (block.link_field ());
}
