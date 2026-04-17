#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/network_filter.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stream.hpp>
#include <nano/lib/vote.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/messages/bulk_pull.hpp>
#include <nano/messages/bulk_pull_account.hpp>
#include <nano/messages/bulk_push.hpp>
#include <nano/messages/confirm.hpp>
#include <nano/messages/deserialize_message.hpp>
#include <nano/messages/frontier_req.hpp>
#include <nano/messages/keepalive.hpp>
#include <nano/messages/message.hpp>
#include <nano/messages/message_header.hpp>
#include <nano/messages/publish.hpp>
#include <nano/messages/telemetry.hpp>
#include <nano/secure/common.hpp>
#include <nano/test_common/random.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

using nano::deserialize_message;
using nano::deserialize_message_status;

namespace
{
// Serialize the message, then split into (header, payload) as nano::deserialize_message expects.
struct split_wire
{
	nano::messages::message_header header;
	std::vector<uint8_t> payload;

	explicit split_wire (nano::messages::message const & msg) :
		header{ msg.header }
	{
		auto bytes = msg.to_bytes ();
		// Strip the leading header bytes — nano::deserialize_message takes payload-only.
		payload.assign (bytes->begin () + nano::messages::message_header::size, bytes->end ());
	}
};
}

TEST (deserialize_message, keepalive_roundtrip_no_collaborators)
{
	nano::messages::keepalive original{ nano::dev::network_params.network };
	split_wire wire{ original };

	auto [msg, status] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);
	ASSERT_EQ (deserialize_message_status::success, status);
	ASSERT_NE (nullptr, msg);
	EXPECT_EQ (*msg->to_bytes (), *original.to_bytes ());
}

TEST (deserialize_message, confirm_req_roundtrip)
{
	nano::messages::confirm_req original{ nano::dev::network_params.network,
		nano::test::random_hash (), nano::test::random_hash () };
	split_wire wire{ original };

	auto [msg, status] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);
	EXPECT_EQ (deserialize_message_status::success, status);
	ASSERT_NE (nullptr, msg);
	EXPECT_EQ (*msg->to_bytes (), *original.to_bytes ());
}

TEST (deserialize_message, telemetry_req_roundtrip)
{
	nano::messages::telemetry_req original{ nano::dev::network_params.network };
	split_wire wire{ original };

	auto [msg, status] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);
	EXPECT_EQ (deserialize_message_status::success, status);
	ASSERT_NE (nullptr, msg);
}

TEST (deserialize_message, bulk_push_roundtrip)
{
	nano::messages::bulk_push original{ nano::dev::network_params.network };
	split_wire wire{ original };

	auto [msg, status] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);
	EXPECT_EQ (deserialize_message_status::success, status);
	ASSERT_NE (nullptr, msg);
}

TEST (deserialize_message, frontier_req_roundtrip)
{
	nano::messages::frontier_req original{ nano::dev::network_params.network };
	split_wire wire{ original };

	auto [msg, status] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);
	EXPECT_EQ (deserialize_message_status::success, status);
	ASSERT_NE (nullptr, msg);
}

TEST (deserialize_message, asc_pull_req_with_payload_roundtrip)
{
	nano::messages::asc_pull_req original{ nano::dev::network_params.network };
	original.id = 7;
	original.type = nano::messages::asc_pull_type::account_info;

	nano::messages::asc_pull_req::account_info_payload payload;
	payload.target = nano::test::random_account ();
	payload.target_type = nano::messages::asc_pull_req::hash_type::account;
	original.payload = payload;
	original.update_header ();

	split_wire wire{ original };

	auto [msg, status] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);
	EXPECT_EQ (deserialize_message_status::success, status);
	ASSERT_NE (nullptr, msg);
}

// Error paths ---------------------------------------------------------------

TEST (deserialize_message, invalid_message_type_returns_status)
{
	// Build a real keepalive header, then poke the type to a reserved invalid value.
	nano::messages::keepalive original{ nano::dev::network_params.network };
	auto header = original.header;
	header.type = nano::messages::message_type::invalid;

	auto [msg, status] = deserialize_message ({}, header, nano::dev::network_params.network);
	EXPECT_EQ (deserialize_message_status::invalid_message_type, status);
	EXPECT_EQ (nullptr, msg);
}

TEST (deserialize_message, truncated_confirm_req_payload)
{
	nano::messages::confirm_req original{ nano::dev::network_params.network,
		nano::test::random_hash (), nano::test::random_hash () };
	split_wire wire{ original };

	// Hand the parser less payload than the header claims.
	ASSERT_GT (wire.payload.size (), 4);
	nano::buffer_view truncated{ wire.payload.data (), wire.payload.size () - 4 };

	auto [msg, status] = deserialize_message (truncated, wire.header, nano::dev::network_params.network);
	EXPECT_EQ (deserialize_message_status::invalid_confirm_req_message, status);
	EXPECT_EQ (nullptr, msg);
}

// A real confirm_ack over the wire should round-trip without any uniquer.
TEST (deserialize_message, confirm_ack_roundtrip_without_uniquer)
{
	nano::keypair kp;
	auto vote = std::make_shared<nano::vote> (
	kp.pub, kp.prv, /*timestamp*/ 0, /*duration*/ 0,
	std::vector<nano::block_hash>{ nano::test::random_hash () });
	nano::messages::confirm_ack original{ nano::dev::network_params.network, vote };
	split_wire wire{ original };

	auto [msg, status] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);
	EXPECT_EQ (deserialize_message_status::success, status);
	ASSERT_NE (nullptr, msg);
	EXPECT_EQ (*msg->to_bytes (), *original.to_bytes ());
}

// Filter detects a duplicate on the second call with identical payload bytes —
// the check runs before deserialization, so we must feed well-formed bytes.
TEST (deserialize_message, network_filter_detects_duplicate_confirm_ack_bytes)
{
	nano::network_filter filter{ 16 };

	nano::keypair kp;
	auto vote = std::make_shared<nano::vote> (
	kp.pub, kp.prv, 0, 0,
	std::vector<nano::block_hash>{ nano::test::random_hash () });
	nano::messages::confirm_ack original{ nano::dev::network_params.network, vote };
	split_wire wire{ original };

	// First pass: filter is fresh — parse should succeed.
	auto [msg1, status1] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network, &filter);
	EXPECT_EQ (deserialize_message_status::success, status1);
	ASSERT_NE (nullptr, msg1);

	// Second pass with the same bytes: filter reports a duplicate immediately.
	auto [msg2, status2] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network, &filter);
	EXPECT_EQ (deserialize_message_status::duplicate_confirm_ack_message, status2);
	EXPECT_EQ (nullptr, msg2);
}

// Without a filter, duplicate detection is skipped — repeated bytes parse each time.
TEST (deserialize_message, no_filter_skips_duplicate_check)
{
	nano::keypair kp;
	auto vote = std::make_shared<nano::vote> (
	kp.pub, kp.prv, 0, 0,
	std::vector<nano::block_hash>{ nano::test::random_hash () });
	nano::messages::confirm_ack original{ nano::dev::network_params.network, vote };
	split_wire wire{ original };

	auto [msg1, status1] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);
	auto [msg2, status2] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);

	EXPECT_EQ (deserialize_message_status::success, status1);
	EXPECT_EQ (deserialize_message_status::success, status2);
	ASSERT_NE (nullptr, msg1);
	ASSERT_NE (nullptr, msg2);
}

// A well-formed publish message carrying the dev genesis block must round-trip.
TEST (deserialize_message, publish_genesis_roundtrip)
{
	auto block = nano::dev::genesis;
	nano::messages::publish original{ nano::dev::network_params.network, block };
	split_wire wire{ original };

	auto [msg, status] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);
	ASSERT_EQ (deserialize_message_status::success, status);
	ASSERT_NE (nullptr, msg);
	EXPECT_EQ (*msg->to_bytes (), *original.to_bytes ());
}

// A publish whose block has zero (therefore invalid) work must be rejected with
// a dedicated status so upstream can tick the right stat.
TEST (deserialize_message, publish_with_insufficient_work_is_rejected)
{
	nano::block_builder builder;
	// State block with zero work — passes structural decode but fails work check.
	auto bad = builder.state ()
			   .account (nano::dev::genesis_key.pub)
			   .previous (nano::dev::genesis->hash ())
			   .representative (nano::dev::genesis_key.pub)
			   .balance (nano::dev::constants.genesis_amount - 1)
			   .link (nano::dev::genesis_key.pub)
			   .sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
			   .work (0)
			   .build ();

	nano::messages::publish original{ nano::dev::network_params.network, bad };
	split_wire wire{ original };

	auto [msg, status] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);
	EXPECT_EQ (deserialize_message_status::insufficient_work, status);
	EXPECT_EQ (nullptr, msg);
}

// Filter dedupe must also kick in for publish (symmetric with confirm_ack).
// Regression guard: the two dedupe paths are separately wired and can drift.
TEST (deserialize_message, network_filter_detects_duplicate_publish_bytes)
{
	nano::network_filter filter{ 16 };

	nano::messages::publish original{ nano::dev::network_params.network, nano::dev::genesis };
	split_wire wire{ original };

	auto [msg1, status1] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network, &filter);
	EXPECT_EQ (deserialize_message_status::success, status1);
	ASSERT_NE (nullptr, msg1);

	auto [msg2, status2] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network, &filter);
	EXPECT_EQ (deserialize_message_status::duplicate_publish_message, status2);
	EXPECT_EQ (nullptr, msg2);
}

// Confirm_ack and publish must live in independent filter buckets — the same
// raw digest shouldn't collide across message types. We verify this indirectly
// by round-tripping both into the same filter and observing both succeed.
TEST (deserialize_message, filter_does_not_conflate_publish_and_confirm_ack)
{
	nano::network_filter filter{ 16 };

	nano::messages::publish publish{ nano::dev::network_params.network, nano::dev::genesis };
	split_wire publish_wire{ publish };

	nano::keypair kp;
	auto vote = std::make_shared<nano::vote> (
	kp.pub, kp.prv, 0, 0,
	std::vector<nano::block_hash>{ nano::test::random_hash () });
	nano::messages::confirm_ack ack{ nano::dev::network_params.network, vote };
	split_wire ack_wire{ ack };

	auto [pmsg, pstatus] = deserialize_message (publish_wire.payload, publish_wire.header, nano::dev::network_params.network, &filter);
	auto [amsg, astatus] = deserialize_message (ack_wire.payload, ack_wire.header, nano::dev::network_params.network, &filter);

	EXPECT_EQ (deserialize_message_status::success, pstatus);
	EXPECT_EQ (deserialize_message_status::success, astatus);
	ASSERT_NE (nullptr, pmsg);
	ASSERT_NE (nullptr, amsg);
}

// Additional roundtrips covering message types that currently lack coverage.
TEST (deserialize_message, bulk_pull_roundtrip)
{
	nano::messages::bulk_pull original{ nano::dev::network_params.network };
	original.start = nano::test::random_account ();
	original.end = nano::test::random_hash ();
	split_wire wire{ original };

	auto [msg, status] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);
	EXPECT_EQ (deserialize_message_status::success, status);
	ASSERT_NE (nullptr, msg);
	EXPECT_EQ (*msg->to_bytes (), *original.to_bytes ());
}

TEST (deserialize_message, bulk_pull_account_roundtrip)
{
	nano::messages::bulk_pull_account original{ nano::dev::network_params.network };
	split_wire wire{ original };

	auto [msg, status] = deserialize_message (wire.payload, wire.header, nano::dev::network_params.network);
	EXPECT_EQ (deserialize_message_status::success, status);
	ASSERT_NE (nullptr, msg);
	EXPECT_EQ (*msg->to_bytes (), *original.to_bytes ());
}
