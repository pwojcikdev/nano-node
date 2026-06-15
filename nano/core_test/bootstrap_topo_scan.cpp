#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/bootstrap/topo_scan.hpp>
#include <nano/secure/network_params.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <set>

using namespace std::chrono_literals;

namespace
{
nano::topo_key key (uint64_t height, uint64_t hash)
{
	return { height, nano::block_hash{ hash } };
}

std::deque<nano::topo_key> keys (std::initializer_list<nano::topo_key> entries)
{
	return { entries.begin (), entries.end () };
}

std::shared_ptr<nano::block> block (uint64_t salt)
{
	nano::state_block_builder builder;
	return builder.account (nano::dev::genesis_key.pub)
	.previous (nano::block_hash{ salt })
	.representative (nano::dev::genesis_key.pub)
	.balance (0)
	.link (nano::link{ salt + 1 })
	.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
	.work (0)
	.build ();
}

nano::topo_key key_for (uint64_t height, std::shared_ptr<nano::block> const & block)
{
	return { height, block->hash () };
}

struct test_context
{
	nano::stats stats{ nano::default_logger () };
	nano::topo_scan_config config;
	nano::bootstrap::topo_scan_engine engine;
	std::chrono::steady_clock::time_point now{};
	std::set<nano::bootstrap::id_t> live_tags;
	std::function<nano::bootstrap::peer_probe_status (std::span<nano::account const>)> peer_status;

	explicit test_context (nano::topo_scan_config config_a = {}) :
		config{ config_a },
		engine{ config, stats },
		peer_status{ [] (std::span<nano::account const>) {
			return nano::bootstrap::peer_probe_status::available;
		} }
	{
		engine.orient ({});
	}

	nano::bootstrap::topo_scan_engine::probes probes ()
	{
		return {
			.peer_status = peer_status,
			.count_inflight = [this] (std::span<nano::bootstrap::id_t const> tag_ids) {
				return static_cast<size_t> (std::count_if (tag_ids.begin (), tag_ids.end (), [this] (auto id) {
					return live_tags.contains (id);
				}));
			},
		};
	}

	std::shared_ptr<nano::bootstrap::topo_round> next_round ()
	{
		auto round = engine.next_round (now, probes ());
		release_assert (round != nullptr);
		return round;
	}

	void reserve (std::shared_ptr<nano::bootstrap::topo_round> const & round, nano::account node_id, nano::bootstrap::id_t id)
	{
		round->reserve (node_id, id, now);
		live_tags.insert (id);
	}

	bool feed_page (nano::bootstrap::id_t id, nano::topo_key const & start, std::deque<nano::topo_key> const & entries)
	{
		live_tags.erase (id);
		return engine.process_page (id, start, entries);
	}
};
}

TEST (bootstrap_topo_round, quorum_done_keeps_smallest_candidates)
{
	nano::topo_scan_config config;
	config.candidates = 3;
	nano::bootstrap::topo_round round{ config, key (10, 1), 2 };

	round.feed (keys ({ key (10, 2), key (10, 5), key (11, 1) }));
	ASSERT_FALSE (round.done ());

	round.feed (keys ({ key (10, 3), key (12, 1) }));
	ASSERT_TRUE (round.done ());
	ASSERT_EQ (round.candidate_count (), 3);
	ASSERT_EQ (round.conclude (), key (10, 5));
}

TEST (bootstrap_topo_round, empty_page)
{
	nano::topo_scan_config config;
	nano::bootstrap::topo_round round{ config, key (10, 1), 2 };

	round.feed ({});
	ASSERT_FALSE (round.empty_page ());

	round.feed ({ key (10, 1) });
	ASSERT_TRUE (round.empty_page ());
	ASSERT_EQ (round.conclude (), key (10, 1));
}

TEST (bootstrap_topo_scan, eager_fanout_tracks_distinct_peers)
{
	nano::topo_scan_config config;
	config.head_count = 1;
	config.consideration_count = 3;
	test_context ctx{ config };

	auto first = ctx.next_round ();
	ASSERT_EQ (first->position (), nano::topo_key{});
	ASSERT_TRUE (first->exclude ().empty ());
	ctx.reserve (first, nano::account{ 10 }, 1);

	auto second = ctx.next_round ();
	ASSERT_EQ (second->position (), first->position ());
	ASSERT_EQ (second->exclude ().size (), 1);
	ASSERT_EQ (second->exclude ()[0], nano::account{ 10 });
	ctx.reserve (second, nano::account{ 11 }, 2);

	auto third = ctx.next_round ();
	ASSERT_EQ (third->exclude ().size (), 2);
	ASSERT_EQ (third->exclude ()[0], nano::account{ 10 });
	ASSERT_EQ (third->exclude ()[1], nano::account{ 11 });
	ctx.reserve (third, nano::account{ 12 }, 3);

	ASSERT_EQ (ctx.engine.next_round (ctx.now, ctx.probes ()), nullptr);
}

TEST (bootstrap_topo_scan, next_round_reuses_unreserved_round)
{
	nano::topo_scan_config config;
	config.head_count = 2;
	test_context ctx{ config };

	auto first = ctx.next_round ();
	auto second = ctx.next_round ();

	ASSERT_EQ (second, first);
	ASSERT_EQ (second->position (), first->position ());
}

TEST (bootstrap_topo_scan, quorum_discovers_fetch_candidates)
{
	auto block1 = block (1);
	auto block2 = block (2);
	auto block3 = block (3);
	auto page = keys ({ key_for (1, block1), key_for (2, block2), key_for (2, block3) });

	nano::topo_scan_config config;
	config.head_count = 1;
	config.consideration_count = 2;
	test_context ctx{ config };

	auto first = ctx.next_round ();
	ctx.reserve (first, nano::account{ 10 }, 1);
	auto second = ctx.next_round ();
	ctx.reserve (second, nano::account{ 11 }, 2);

	ASSERT_TRUE (ctx.feed_page (1, first->position (), page));
	ASSERT_TRUE (ctx.feed_page (2, first->position (), page));
	ctx.engine.settle (ctx.now, ctx.probes ());

	auto hashes = ctx.engine.next_fetch_candidates (ctx.now, 10);
	ASSERT_EQ (hashes.size (), 3);
	ASSERT_EQ (hashes[0], block1->hash ());
	ASSERT_EQ (hashes[1], block2->hash ());
	ASSERT_EQ (hashes[2], block3->hash ());
}

TEST (bootstrap_topo_scan, fetched_blocks_submit_in_topo_order)
{
	auto block1 = block (11);
	auto block2 = block (12);
	auto page = keys ({ key_for (1, block1), key_for (2, block2) });

	nano::topo_scan_config config;
	config.head_count = 1;
	config.consideration_count = 1;
	test_context ctx{ config };

	auto round = ctx.next_round ();
	ctx.reserve (round, nano::account{ 10 }, 1);
	ASSERT_TRUE (ctx.feed_page (1, round->position (), page));
	ctx.engine.settle (ctx.now, ctx.probes ());

	auto hashes = ctx.engine.next_fetch_candidates (ctx.now, 10);
	ctx.engine.commit_fetch (hashes, {}, 99, ctx.now);
	ASSERT_TRUE (ctx.engine.process_blocks (99, { block2, block1 }, ctx.now));

	auto submit = ctx.engine.next_submit_batch (ctx.now, 10);
	ASSERT_EQ (submit.size (), 2);
	ASSERT_EQ (submit[0]->hash (), block1->hash ());
	ASSERT_EQ (submit[1]->hash (), block2->hash ());
}

TEST (bootstrap_topo_scan, frontier_gap_is_retained_and_retried)
{
	auto block1 = block (21);
	auto page = keys ({ key_for (1, block1) });

	nano::topo_scan_config config;
	config.head_count = 1;
	config.consideration_count = 1;
	config.retry_interval = 5s;
	test_context ctx{ config };

	auto round = ctx.next_round ();
	ctx.reserve (round, nano::account{ 10 }, 1);
	ASSERT_TRUE (ctx.feed_page (1, round->position (), page));
	ctx.engine.settle (ctx.now, ctx.probes ());

	auto hashes = ctx.engine.next_fetch_candidates (ctx.now, 10);
	ctx.engine.commit_fetch (hashes, {}, 99, ctx.now);
	ASSERT_TRUE (ctx.engine.process_blocks (99, { block1 }, ctx.now));

	auto submit = ctx.engine.next_submit_batch (ctx.now, 10);
	ASSERT_EQ (submit.size (), 1);
	ASSERT_TRUE (ctx.engine.inspect (block1->hash (), nano::block_status::gap_previous, ctx.now));
	ASSERT_EQ (ctx.engine.frontier_gap_count (), 1);
	ASSERT_FALSE (ctx.engine.submit_ready (ctx.now));

	ctx.now += config.retry_interval;
	ASSERT_TRUE (ctx.engine.submit_ready (ctx.now));
	auto retry = ctx.engine.next_submit_batch (ctx.now, 10);
	ASSERT_EQ (retry.size (), 1);
	ASSERT_EQ (retry.front ()->hash (), block1->hash ());

	ASSERT_TRUE (ctx.engine.inspect (block1->hash (), nano::block_status::progress, ctx.now));
	ASSERT_EQ (ctx.engine.frontier_gap_count (), 0);
	ASSERT_EQ (ctx.engine.cursor (), key_for (1, block1));
	ASSERT_EQ (ctx.engine.queued (), 0);
}

TEST (bootstrap_topo_scan, empty_tip_requires_two_rounds)
{
	nano::topo_scan_config config;
	config.head_count = 1;
	config.consideration_count = 1;
	config.cooldown = 1s;
	test_context ctx{ config };

	auto first = ctx.next_round ();
	ctx.reserve (first, nano::account{ 10 }, 1);
	ASSERT_TRUE (ctx.feed_page (1, first->position (), {}));
	ctx.engine.settle (ctx.now, ctx.probes ());
	ASSERT_FALSE (ctx.engine.caught_up ());

	ctx.now += config.cooldown;
	auto second = ctx.next_round ();
	ctx.reserve (second, nano::account{ 11 }, 2);
	ASSERT_TRUE (ctx.feed_page (2, second->position (), {}));
	ctx.engine.settle (ctx.now, ctx.probes ());
	ASSERT_TRUE (ctx.engine.caught_up ());
}
