#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/frontier_scan.hpp>
#include <nano/node/bootstrap/verify.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/test_common/ledger_context.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace nano::bootstrap;

namespace
{
std::deque<std::pair<nano::account, nano::block_hash>> make_frontiers (std::initializer_list<uint64_t> accounts)
{
	std::deque<std::pair<nano::account, nano::block_hash>> result;
	for (auto account : accounts)
	{
		result.push_back ({ nano::account{ account }, nano::block_hash{ account } });
	}
	return result;
}
}

/*
 * Response verification: frontiers
 */

TEST (bootstrap_verify, frontiers_empty)
{
	frontiers_query query{ .start = nano::account{ 1 }, .count = 1000 };
	nano::messages::asc_pull_ack::frontiers_payload response;
	ASSERT_EQ (verify (response, query), verify_result::nothing_new);
}

TEST (bootstrap_verify, frontiers_ok)
{
	frontiers_query query{ .start = nano::account{ 1 }, .count = 1000 };
	nano::messages::asc_pull_ack::frontiers_payload response;
	response.frontiers = make_frontiers ({ 2, 3, 5 });
	ASSERT_EQ (verify (response, query), verify_result::ok);
}

TEST (bootstrap_verify, frontiers_unordered)
{
	frontiers_query query{ .start = nano::account{ 1 }, .count = 1000 };
	nano::messages::asc_pull_ack::frontiers_payload response;
	response.frontiers = make_frontiers ({ 5, 3 });
	ASSERT_EQ (verify (response, query), verify_result::invalid);
}

TEST (bootstrap_verify, frontiers_duplicate)
{
	frontiers_query query{ .start = nano::account{ 1 }, .count = 1000 };
	nano::messages::asc_pull_ack::frontiers_payload response;
	response.frontiers = make_frontiers ({ 3, 3 });
	ASSERT_EQ (verify (response, query), verify_result::invalid);
}

TEST (bootstrap_verify, frontiers_below_start)
{
	frontiers_query query{ .start = nano::account{ 10 }, .count = 1000 };
	nano::messages::asc_pull_ack::frontiers_payload response;
	response.frontiers = make_frontiers ({ 5, 11 });
	ASSERT_EQ (verify (response, query), verify_result::invalid);
}

/*
 * Response verification: blocks
 */

namespace
{
// A small chain of state blocks; verification checks hashes and links, not signatures or work
std::deque<std::shared_ptr<nano::block>> make_chain (size_t count)
{
	std::deque<std::shared_ptr<nano::block>> blocks;
	nano::keypair key;
	nano::block_builder builder;
	nano::block_hash previous{ 0 };
	for (size_t n = 0; n < count; ++n)
	{
		auto block = builder.state ()
					 .make_block ()
					 .account (key.pub)
					 .previous (previous)
					 .representative (key.pub)
					 .balance (count - n)
					 .link (0)
					 .sign (key.prv, key.pub)
					 .work (0)
					 .build ();
		previous = block->hash ();
		blocks.push_back (block);
	}
	return blocks;
}
}

TEST (bootstrap_verify, blocks_empty)
{
	blocks_query query{ .account = nano::account{ 1 }, .start = nano::account{ 1 }, .count = 128, .type = query_type::blocks_by_account };
	nano::messages::asc_pull_ack::blocks_payload response;
	ASSERT_EQ (verify (response, query), verify_result::nothing_new);
}

TEST (bootstrap_verify, blocks_chain_ok)
{
	auto chain = make_chain (3);
	blocks_query query{ .account = chain.front ()->account_field ().value (), .start = chain.front ()->hash (), .count = 128, .type = query_type::blocks_by_hash };
	nano::messages::asc_pull_ack::blocks_payload response;
	response.blocks = chain;
	ASSERT_EQ (verify (response, query), verify_result::ok);
}

TEST (bootstrap_verify, blocks_single_echo)
{
	auto chain = make_chain (1);
	blocks_query query{ .account = chain.front ()->account_field ().value (), .start = chain.front ()->hash (), .count = 128, .type = query_type::blocks_by_hash };
	nano::messages::asc_pull_ack::blocks_payload response;
	response.blocks = chain;
	ASSERT_EQ (verify (response, query), verify_result::nothing_new);
}

TEST (bootstrap_verify, blocks_count_overflow)
{
	auto chain = make_chain (5);
	blocks_query query{ .account = chain.front ()->account_field ().value (), .start = chain.front ()->hash (), .count = 3, .type = query_type::blocks_by_hash };
	nano::messages::asc_pull_ack::blocks_payload response;
	response.blocks = chain;
	ASSERT_EQ (verify (response, query), verify_result::invalid);
}

TEST (bootstrap_verify, blocks_start_mismatch)
{
	auto chain = make_chain (3);
	blocks_query query{ .account = chain.front ()->account_field ().value (), .start = nano::block_hash{ 12345 }, .count = 128, .type = query_type::blocks_by_hash };
	nano::messages::asc_pull_ack::blocks_payload response;
	response.blocks = chain;
	ASSERT_EQ (verify (response, query), verify_result::invalid);
}

TEST (bootstrap_verify, blocks_chain_break)
{
	auto chain = make_chain (3);
	auto foreign = make_chain (1);
	chain[1] = foreign.front (); // Break the previous () linkage
	blocks_query query{ .account = chain.front ()->account_field ().value (), .start = chain.front ()->hash (), .count = 128, .type = query_type::blocks_by_hash };
	nano::messages::asc_pull_ack::blocks_payload response;
	response.blocks = chain;
	ASSERT_EQ (verify (response, query), verify_result::invalid);
}

TEST (bootstrap_verify, blocks_by_account_mismatch)
{
	auto chain = make_chain (2);
	blocks_query query{ .account = nano::account{ 999 }, .start = nano::account{ 999 }, .count = 128, .type = query_type::blocks_by_account };
	nano::messages::asc_pull_ack::blocks_payload response;
	response.blocks = chain;
	ASSERT_EQ (verify (response, query), verify_result::invalid);
}

/*
 * Frontier round candidate voting
 */

namespace
{
nano::frontier_scan_config round_config (unsigned consideration_count = 2, std::size_t candidates = 1000)
{
	nano::frontier_scan_config config;
	config.consideration_count = consideration_count;
	config.candidates = candidates;
	return config;
}

nano::account const range_end{ 1000000 };
}

TEST (bootstrap_frontier_round, done_after_quorum)
{
	auto config = round_config (2);
	frontier_round round{ config, nano::account{ 10 }, range_end };

	round.feed (make_frontiers ({ 11, 12 }));
	ASSERT_FALSE (round.settled ()); // One sample is not enough

	round.feed (make_frontiers ({ 11, 15 }));
	ASSERT_TRUE (round.done ());
	ASSERT_TRUE (round.settled ());
	ASSERT_EQ (round.conclude (), nano::account{ 15 }); // Largest candidate
}

TEST (bootstrap_frontier_round, trims_candidates)
{
	auto config = round_config (1, /* candidates */ 2);
	frontier_round round{ config, nano::account{ 10 }, range_end };

	round.feed (make_frontiers ({ 11, 12, 13, 14 }));
	ASSERT_TRUE (round.done ());
	ASSERT_EQ (round.candidate_count (), 2);
	ASSERT_EQ (round.conclude (), nano::account{ 12 }); // The smallest candidates are kept, advance is bounded
}

TEST (bootstrap_frontier_round, candidates_must_advance)
{
	auto config = round_config (1);
	frontier_round round{ config, nano::account{ 10 }, range_end };

	round.feed (make_frontiers ({ 10 })); // Equal to the position, does not advance the scan
	ASSERT_EQ (round.candidate_count (), 0);
	ASSERT_FALSE (round.done ());
}

TEST (bootstrap_frontier_round, empty_range)
{
	auto config = round_config (2);
	frontier_round round{ config, nano::account{ 10 }, range_end };

	for (int n = 0; n < 4; ++n) // Takes consideration_count * 2 empty samples to give up
	{
		ASSERT_FALSE (round.empty_range ());
		round.feed ({});
	}
	ASSERT_TRUE (round.empty_range ());
	ASSERT_TRUE (round.settled ());
	ASSERT_EQ (round.conclude (), range_end);
}

TEST (bootstrap_frontier_round, partial_conclusion)
{
	auto config = round_config (4);
	frontier_round round{ config, nano::account{ 10 }, range_end };

	// A single sample with candidates does not settle the round, but an early conclusion
	// (e.g. when all distinct peers were already sampled) still advances with what was gathered
	round.feed (make_frontiers ({ 42 }));
	ASSERT_FALSE (round.settled ());
	ASSERT_EQ (round.conclude (), nano::account{ 42 });

	// An early conclusion of all-empty samples treats the range as exhausted
	frontier_round empty_round{ config, nano::account{ 10 }, range_end };
	empty_round.feed ({});
	ASSERT_FALSE (empty_round.settled ());
	ASSERT_EQ (empty_round.conclude (), range_end);
}

TEST (bootstrap_frontier_round, nothing_learned)
{
	auto config = round_config (2);
	frontier_round round{ config, nano::account{ 10 }, range_end };
	ASSERT_EQ (round.conclude (), std::nullopt); // No samples completed, the position should be retried
}

/*
 * Frontier classification against the ledger
 */

TEST (bootstrap_classify, frontier_matches)
{
	auto ctx = nano::test::ledger_send_receive ();
	auto transaction = ctx.ledger ().tx_begin_read ();

	auto const head = ctx.blocks ().back ()->hash (); // The local frontier of the genesis account
	auto result = classify_frontiers (ctx.ledger (), transaction, { { nano::dev::genesis_key.pub, head } });

	ASSERT_TRUE (result.prioritize.empty ()); // Account exists and the frontier is up to date
	ASSERT_EQ (result.outdated, 0);
	ASSERT_EQ (result.pending, 0);
}

TEST (bootstrap_classify, outdated_frontier)
{
	auto ctx = nano::test::ledger_send_receive ();
	auto transaction = ctx.ledger ().tx_begin_read ();

	// Known account advertising an unknown frontier block
	auto result = classify_frontiers (ctx.ledger (), transaction, { { nano::dev::genesis_key.pub, nano::block_hash{ 12345 } } });

	ASSERT_EQ (result.prioritize.size (), 1);
	ASSERT_EQ (result.prioritize.front (), nano::dev::genesis_key.pub);
	ASSERT_EQ (result.outdated, 1);
}

TEST (bootstrap_classify, peer_behind)
{
	auto ctx = nano::test::ledger_send_receive ();
	auto transaction = ctx.ledger ().tx_begin_read ();

	// The advertised frontier mismatches but exists locally: the peer is behind us, nothing to do
	auto const old_head = ctx.blocks ().front ()->hash ();
	auto result = classify_frontiers (ctx.ledger (), transaction, { { nano::dev::genesis_key.pub, old_head } });

	ASSERT_TRUE (result.prioritize.empty ());
	ASSERT_EQ (result.outdated, 0);
}

TEST (bootstrap_classify, pending)
{
	// Build a ledger with an unreceived send to a new account
	nano::keypair key;
	nano::work_pool pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };
	nano::block_builder builder;
	auto send = builder.state ()
				.make_block ()
				.account (nano::dev::genesis_key.pub)
				.previous (nano::dev::genesis->hash ())
				.representative (nano::dev::genesis_key.pub)
				.balance (nano::dev::constants.genesis_amount - 1)
				.link (key.pub)
				.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
				.work (*pool.generate (nano::dev::genesis->hash ()))
				.build ();

	nano::test::ledger_context ctx{ { send } };
	auto transaction = ctx.ledger ().tx_begin_read ();

	// The account is absent locally but has a pending entry, prioritize it
	auto result = classify_frontiers (ctx.ledger (), transaction, { { key.pub, nano::block_hash{ 1 } } });

	ASSERT_EQ (result.prioritize.size (), 1);
	ASSERT_EQ (result.prioritize.front (), key.pub);
	ASSERT_EQ (result.pending, 1);
}

TEST (bootstrap_classify, unknown_account)
{
	auto ctx = nano::test::ledger_send_receive ();
	auto transaction = ctx.ledger ().tx_begin_read ();

	// Unknown account with no pending entries cannot be prioritized yet
	auto result = classify_frontiers (ctx.ledger (), transaction, { { nano::account{ 99999 }, nano::block_hash{ 1 } } });

	ASSERT_TRUE (result.prioritize.empty ());
}
