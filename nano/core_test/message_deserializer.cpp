#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/stream.hpp>
#include <nano/lib/vote.hpp>
#include <nano/messages/publish.hpp>
#include <nano/node/transport/message_deserializer.hpp>
#include <nano/secure/common.hpp>
#include <nano/test_common/random.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/asio/error.hpp>
#include <boost/endian/conversion.hpp>

#include <algorithm>
#include <memory>
#include <vector>

// Test the successful cases for message_deserializer, checking the supported message types and
// the integrity of the deserialized outcome.
template <class message_type>
auto message_deserializer_success_checker (message_type & message_original) -> void
{
	// Dependencies for the message deserializer.
	nano::network_filter filter (1);
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer;

	// Data used to simulate the incoming buffer to be deserialized, the offset tracks how much has been read from the input_source
	// as the read function is called first to read the header, then called again to read the payload.
	std::vector<uint8_t> input_source;
	std::size_t offset{ 0 };

	// Message Deserializer with the query function tweaked to read from the `input_source`.
	auto const message_deserializer = std::make_shared<nano::transport::message_deserializer> (nano::dev::network_params.network, filter, block_uniquer, vote_uniquer,
	[&input_source, &offset] (std::shared_ptr<std::vector<uint8_t>> const & data_a, std::size_t size_a, std::function<void (boost::system::error_code const &, std::size_t)> callback_a) {
		debug_assert (input_source.size () >= size_a);
		data_a->resize (size_a);
		auto const copy_start = input_source.begin () + offset;
		std::copy (copy_start, copy_start + size_a, data_a->data ());
		offset += size_a;
		callback_a (boost::system::errc::make_error_code (boost::system::errc::success), size_a);
	});

	// Generating the values for the `input_source`.
	{
		nano::vectorstream stream (input_source);
		message_original.serialize (stream);
	}

	// Deserializing and testing the success path.
	message_deserializer->read (
	[&message_original] (boost::system::error_code ec_a, std::unique_ptr<nano::messages::message> message_a) {
		auto deserialized_message = dynamic_cast<message_type *> (message_a.get ());
		// Ensure the message type is supported.
		ASSERT_NE (deserialized_message, nullptr);
		auto deserialized_bytes = deserialized_message->to_bytes ();
		auto original_bytes = message_original.to_bytes ();
		// Ensure the integrity of the deserialized message.
		ASSERT_EQ (*deserialized_bytes, *original_bytes);
	});
	// This is a sanity test, to ensure the successful deserialization case passes.
	ASSERT_EQ (message_deserializer->status, nano::transport::parse_status::success);
}

TEST (message_deserializer, exact_confirm_ack)
{
	nano::test::system system{ 1 };
	nano::block_builder builder;
	auto block = builder
				 .send ()
				 .previous (1)
				 .destination (1)
				 .balance (2)
				 .sign (nano::keypair ().prv, 4)
				 .work (*system.work.generate (nano::root (1)))
				 .build ();
	auto vote (std::make_shared<nano::vote> (0, nano::keypair ().prv, 0, 0, std::vector<nano::block_hash>{ block->hash () }));
	nano::messages::confirm_ack message{ nano::dev::network_params.network, vote };

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_confirm_req_hash)
{
	nano::test::system system{ 1 };
	nano::block_builder builder;
	auto block = builder
				 .send ()
				 .previous (1)
				 .destination (1)
				 .balance (2)
				 .sign (nano::keypair ().prv, 4)
				 .work (*system.work.generate (nano::root (1)))
				 .build ();
	// This test differs from the previous `exact_confirm_req` because this tests the confirm_req created from the block hash.
	nano::messages::confirm_req message{ nano::dev::network_params.network, block->hash (), block->root () };

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_publish)
{
	nano::test::system system{ 1 };
	nano::block_builder builder;
	auto block = builder
				 .send ()
				 .previous (1)
				 .destination (1)
				 .balance (2)
				 .sign (nano::keypair ().prv, 4)
				 .work (*system.work.generate (nano::root (1)))
				 .build ();
	nano::messages::publish message{ nano::dev::network_params.network, block };

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_keepalive)
{
	nano::messages::keepalive message{ nano::dev::network_params.network };

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_frontier_req)
{
	nano::messages::frontier_req message{ nano::dev::network_params.network };
	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_telemetry_req)
{
	nano::messages::telemetry_req message{ nano::dev::network_params.network };
	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_telemetry_ack)
{
	nano::messages::telemetry_data data;
	data.unknown_data.push_back (0xFF);

	nano::messages::telemetry_ack message{ nano::dev::network_params.network, data };
	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_bulk_pull)
{
	nano::messages::bulk_pull message{ nano::dev::network_params.network };
	message.header.flag_set (nano::messages::message_header::bulk_pull_ascending_flag);

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_bulk_pull_account)
{
	nano::messages::bulk_pull_account message{ nano::dev::network_params.network };
	message.flags = nano::messages::bulk_pull_account_flags::pending_address_only;

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_bulk_push)
{
	nano::messages::bulk_push message{ nano::dev::network_params.network };
	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_node_id_handshake)
{
	nano::messages::node_id_handshake message{ nano::dev::network_params.network, std::nullopt, std::nullopt };
	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_asc_pull_req)
{
	nano::messages::asc_pull_req message{ nano::dev::network_params.network };

	// The asc_pull_req checks for the message fields and the payload to be filled.
	message.id = 7;
	message.type = nano::messages::asc_pull_type::account_info;

	nano::messages::asc_pull_req::account_info_payload message_payload;
	message_payload.target = nano::test::random_account ();
	message_payload.target_type = nano::messages::asc_pull_req::hash_type::account;

	message.payload = message_payload;
	message.update_header ();

	message_deserializer_success_checker<decltype (message)> (message);
}

TEST (message_deserializer, exact_asc_pull_ack)
{
	nano::messages::asc_pull_ack message{ nano::dev::network_params.network };

	// The asc_pull_ack checks for the message fields and the payload to be filled.
	message.id = 11;
	message.type = nano::messages::asc_pull_type::account_info;

	nano::messages::asc_pull_ack::account_info_payload message_payload;
	message_payload.account = nano::test::random_account ();
	message_payload.account_open = nano::test::random_hash ();
	message_payload.account_head = nano::test::random_hash ();
	message_payload.account_block_count = 932932132;
	message_payload.account_conf_frontier = nano::test::random_hash ();
	message_payload.account_conf_height = 847312;

	message.payload = message_payload;
	message.update_header ();

	message_deserializer_success_checker<decltype (message)> (message);
}

// ---------------------------------------------------------------------------
// Error-path tests.
//
// The success tests above exercise one happy-path round trip per message type
// with a cooperative read_query that always delivers exactly the requested
// number of bytes. The streaming deserializer, however, has a rich status
// enum (nano::transport::parse_status) and a set of error conditions that are
// otherwise unreachable from the node-level integration tests without racy
// stats assertions. The tests below drive the deserializer with a scripted
// sequence of reads so each error branch can be asserted directly.
// ---------------------------------------------------------------------------

namespace
{
// One step of the scripted read_query: bytes to deliver + error code to
// report back to the deserializer. `size_override` lets us report a byte
// count different from what was actually copied (to simulate a short read).
struct read_step
{
	std::vector<uint8_t> data;
	boost::system::error_code ec{};
	std::size_t size_override{ 0 }; // 0 → report data.size ()
};

// Captures the terminal callback invocation.
struct capture
{
	boost::system::error_code ec;
	std::unique_ptr<nano::messages::message> message;
	bool called{ false };
};

// Runs one message_deserializer::read () against a scripted sequence of
// read_query invocations. The callbacks are invoked synchronously inside
// read_query so by the time `read` returns, `out` has been populated.
std::shared_ptr<nano::transport::message_deserializer> run_deserialize (
std::vector<read_step> script,
capture & out,
nano::network_filter & filter,
nano::block_uniquer & block_uniquer,
nano::vote_uniquer & vote_uniquer)
{
	auto idx = std::make_shared<std::size_t> (0);
	auto script_ptr = std::make_shared<std::vector<read_step>> (std::move (script));

	auto d = std::make_shared<nano::transport::message_deserializer> (
	nano::dev::network_params.network, filter, block_uniquer, vote_uniquer,
	[idx, script_ptr] (std::shared_ptr<std::vector<uint8_t>> const & buf, std::size_t size, std::function<void (boost::system::error_code const &, std::size_t)> cb) {
		debug_assert (*idx < script_ptr->size ());
		auto const & step = (*script_ptr)[(*idx)++];
		buf->resize (size);
		auto const to_copy = std::min (size, step.data.size ());
		std::copy_n (step.data.begin (), to_copy, buf->data ());
		auto const reported = step.size_override != 0 ? step.size_override : step.data.size ();
		cb (step.ec, reported);
	});

	d->read ([&out] (boost::system::error_code ec, std::unique_ptr<nano::messages::message> msg) {
		out.ec = ec;
		out.message = std::move (msg);
		out.called = true;
	});
	return d;
}

// Build an 8-byte wire header with arbitrary fields. Matches the layout in
// nano::messages::message_header::serialize (): network (u16 big-endian),
// version_max, version_using, version_min, type, extensions (u16 little-endian).
std::vector<uint8_t> make_header (nano::network_type network,
uint8_t version_using,
uint8_t version_min,
nano::messages::message_type type,
uint16_t extensions = 0)
{
	std::vector<uint8_t> out;
	nano::vectorstream stream (out);
	nano::write (stream, boost::endian::native_to_big (static_cast<uint16_t> (network)));
	nano::write (stream, nano::dev::network_params.network.protocol_version); // version_max
	nano::write (stream, version_using);
	nano::write (stream, version_min);
	nano::write (stream, type);
	nano::write (stream, static_cast<uint16_t> (extensions));
	return out;
}
}

// The terminal callback must propagate the underlying I/O error verbatim when
// the header read itself fails.
TEST (message_deserializer, header_read_error_is_propagated)
{
	nano::network_filter filter{ 1 };
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer;
	capture out;

	auto d = run_deserialize ({ read_step{ {}, boost::asio::error::eof, 0 } },
	out, filter, block_uniquer, vote_uniquer);

	ASSERT_TRUE (out.called);
	EXPECT_EQ (out.ec, boost::asio::error::eof);
	EXPECT_EQ (out.message, nullptr);
	// Status is cleared to `none` at the start of read() and is not touched when
	// the I/O layer surfaces an error — status is reserved for parser outcomes.
	EXPECT_EQ (d->status, nano::transport::parse_status::none);
}

// A successful read that returns fewer bytes than requested for the header
// must be surfaced as a hard fault — we cannot parse a partial header.
TEST (message_deserializer, header_short_read_fails_with_fault)
{
	nano::network_filter filter{ 1 };
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer;
	capture out;

	auto d = run_deserialize ({ read_step{ std::vector<uint8_t>{ 0x00, 0x00, 0x00 }, boost::system::error_code{}, 3 } },
	out, filter, block_uniquer, vote_uniquer);

	ASSERT_TRUE (out.called);
	EXPECT_EQ (out.ec, boost::asio::error::fault);
	EXPECT_EQ (out.message, nullptr);
	EXPECT_EQ (d->status, nano::transport::parse_status::none);
}

// A header whose network byte does not match our current network must set
// the `invalid_network` status — this is the front-line defense against
// cross-network confusion attacks.
TEST (message_deserializer, header_rejects_wrong_network)
{
	nano::network_filter filter{ 1 };
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer;
	capture out;

	auto header = make_header (nano::network_type::nano_live_network,
	nano::dev::network_params.network.protocol_version,
	nano::dev::network_params.network.protocol_version_min,
	nano::messages::message_type::keepalive);

	auto d = run_deserialize ({ read_step{ header, {}, 0 } }, out, filter, block_uniquer, vote_uniquer);

	ASSERT_TRUE (out.called);
	EXPECT_EQ (out.ec, boost::asio::error::fault);
	EXPECT_EQ (out.message, nullptr);
	EXPECT_EQ (d->status, nano::transport::parse_status::invalid_network);
}

// A header whose `version_using` is below our minimum accepted version must
// set `outdated_version`. This is the symmetric of the active version check.
TEST (message_deserializer, header_rejects_outdated_version)
{
	nano::network_filter filter{ 1 };
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer;
	capture out;

	auto const outdated = static_cast<uint8_t> (nano::dev::network_params.network.protocol_version_min - 1);
	auto header = make_header (nano::dev::network_params.network.current_network,
	outdated,
	nano::dev::network_params.network.protocol_version_min,
	nano::messages::message_type::keepalive);

	auto d = run_deserialize ({ read_step{ header, {}, 0 } }, out, filter, block_uniquer, vote_uniquer);

	ASSERT_TRUE (out.called);
	EXPECT_EQ (out.ec, boost::asio::error::fault);
	EXPECT_EQ (out.message, nullptr);
	EXPECT_EQ (d->status, nano::transport::parse_status::outdated_version);
}

// A header whose `type` is not a known message type (0x1 = `not_a_type`, used
// as a sentinel value) must be rejected with `invalid_header`.
TEST (message_deserializer, header_rejects_unknown_message_type)
{
	nano::network_filter filter{ 1 };
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer;
	capture out;

	auto header = make_header (nano::dev::network_params.network.current_network,
	nano::dev::network_params.network.protocol_version,
	nano::dev::network_params.network.protocol_version_min,
	nano::messages::message_type::not_a_type);

	auto d = run_deserialize ({ read_step{ header, {}, 0 } }, out, filter, block_uniquer, vote_uniquer);

	ASSERT_TRUE (out.called);
	EXPECT_EQ (out.ec, boost::asio::error::fault);
	EXPECT_EQ (out.message, nullptr);
	EXPECT_EQ (d->status, nano::transport::parse_status::invalid_header);
}

// Header read succeeds, but the follow-up payload read surfaces an I/O error.
// The error code must flow back to the terminal callback verbatim and the
// status must remain `none` (the parser never ran).
TEST (message_deserializer, payload_read_error_is_propagated)
{
	nano::network_filter filter{ 1 };
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer;
	capture out;

	// Serialize a real publish so the payload-length computation in the header
	// is internally consistent. We only deliver the header successfully; the
	// payload step synthesizes a connection-reset.
	nano::messages::publish publish{ nano::dev::network_params.network, nano::dev::genesis };
	std::vector<uint8_t> wire;
	{
		nano::vectorstream stream{ wire };
		publish.serialize (stream);
	}
	std::vector<uint8_t> header_bytes (wire.begin (), wire.begin () + 8);

	auto d = run_deserialize ({
							  read_step{ header_bytes, {}, 0 },
							  read_step{ {}, boost::asio::error::connection_reset, 0 },
							  },
	out, filter, block_uniquer, vote_uniquer);

	ASSERT_TRUE (out.called);
	EXPECT_EQ (out.ec, boost::asio::error::connection_reset);
	EXPECT_EQ (out.message, nullptr);
	EXPECT_EQ (d->status, nano::transport::parse_status::none);
}

// Header read succeeds, but the payload read returns fewer bytes than
// promised. The deserializer must fail hard rather than hand a truncated
// buffer to the parser.
TEST (message_deserializer, payload_short_read_fails_with_fault)
{
	nano::network_filter filter{ 1 };
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer;
	capture out;

	nano::messages::publish publish{ nano::dev::network_params.network, nano::dev::genesis };
	std::vector<uint8_t> wire;
	{
		nano::vectorstream stream{ wire };
		publish.serialize (stream);
	}
	std::vector<uint8_t> header_bytes (wire.begin (), wire.begin () + 8);
	// Deliver only half of the payload but keep the error code clean, so the
	// deserializer has to catch the short-read itself.
	auto const expected_payload = wire.size () - 8;
	std::vector<uint8_t> truncated_payload (wire.begin () + 8, wire.begin () + 8 + expected_payload / 2);

	auto d = run_deserialize ({
							  read_step{ header_bytes, {}, 0 },
							  read_step{ truncated_payload, {}, truncated_payload.size () },
							  },
	out, filter, block_uniquer, vote_uniquer);

	ASSERT_TRUE (out.called);
	EXPECT_EQ (out.ec, boost::asio::error::fault);
	EXPECT_EQ (out.message, nullptr);
	EXPECT_EQ (d->status, nano::transport::parse_status::none);
}

// A structurally valid publish carrying a block with zero work must be
// rejected with `insufficient_work`. This is a security-relevant path: if the
// work check regressed, the node would admit PoW-less blocks.
TEST (message_deserializer, publish_with_insufficient_work_is_rejected)
{
	nano::network_filter filter{ 1 };
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer;
	capture out;

	nano::block_builder builder;
	auto bad = builder.state ()
			   .account (nano::dev::genesis_key.pub)
			   .previous (nano::dev::genesis->hash ())
			   .representative (nano::dev::genesis_key.pub)
			   .balance (nano::dev::constants.genesis_amount - 1)
			   .link (nano::dev::genesis_key.pub)
			   .sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
			   .work (0)
			   .build ();

	nano::messages::publish publish{ nano::dev::network_params.network, bad };
	std::vector<uint8_t> wire;
	{
		nano::vectorstream stream{ wire };
		publish.serialize (stream);
	}
	std::vector<uint8_t> header_bytes (wire.begin (), wire.begin () + 8);
	std::vector<uint8_t> payload_bytes (wire.begin () + 8, wire.end ());

	auto d = run_deserialize ({
							  read_step{ header_bytes, {}, 0 },
							  read_step{ payload_bytes, {}, 0 },
							  },
	out, filter, block_uniquer, vote_uniquer);

	ASSERT_TRUE (out.called);
	// Soft error: no I/O error, no message, but status encodes the reason.
	EXPECT_FALSE (out.ec);
	EXPECT_EQ (out.message, nullptr);
	EXPECT_EQ (d->status, nano::transport::parse_status::insufficient_work);
}

// Seeing the same publish twice through the same network_filter must flip the
// second one into `duplicate_publish_message` — the dedupe happens before the
// payload is even handed to the parser. Mirrors the stateless
// deserialize_message test but on the streaming path.
TEST (message_deserializer, duplicate_publish_is_filtered)
{
	nano::network_filter filter{ 16 };
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer;

	nano::messages::publish publish{ nano::dev::network_params.network, nano::dev::genesis };
	std::vector<uint8_t> wire;
	{
		nano::vectorstream stream{ wire };
		publish.serialize (stream);
	}
	std::vector<uint8_t> header_bytes (wire.begin (), wire.begin () + 8);
	std::vector<uint8_t> payload_bytes (wire.begin () + 8, wire.end ());

	// First pass: fresh bytes, should succeed with a parsed message.
	capture first;
	auto d1 = run_deserialize ({
							   read_step{ header_bytes, {}, 0 },
							   read_step{ payload_bytes, {}, 0 },
							   },
	first, filter, block_uniquer, vote_uniquer);
	ASSERT_TRUE (first.called);
	EXPECT_FALSE (first.ec);
	ASSERT_NE (first.message, nullptr);
	EXPECT_EQ (d1->status, nano::transport::parse_status::success);

	// Second pass: identical bytes, filter should catch it before decode.
	capture second;
	auto d2 = run_deserialize ({
							   read_step{ header_bytes, {}, 0 },
							   read_step{ payload_bytes, {}, 0 },
							   },
	second, filter, block_uniquer, vote_uniquer);
	ASSERT_TRUE (second.called);
	EXPECT_FALSE (second.ec);
	EXPECT_EQ (second.message, nullptr);
	EXPECT_EQ (d2->status, nano::transport::parse_status::duplicate_publish_message);
}
