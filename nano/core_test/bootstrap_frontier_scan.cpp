#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/bootstrap/frontier_scan.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <set>

using namespace std::chrono_literals;

namespace
{
std::deque<std::pair<nano::account, nano::block_hash>> frontiers (std::initializer_list<uint64_t> accounts)
{
	std::deque<std::pair<nano::account, nano::block_hash>> result;
	for (auto account : accounts)
	{
		result.push_back ({ nano::account{ account }, nano::block_hash{ account } });
	}
	return result;
}

struct test_context
{
	nano::stats stats{ nano::default_logger () };
	nano::frontier_scan_config config;
	nano::bootstrap::frontier_scan_engine engine;
	std::chrono::steady_clock::time_point now{};
	std::set<nano::bootstrap::id_t> live_tags;
	std::function<nano::bootstrap::peer_probe_status (std::span<nano::account const>)> peer_status;

	explicit test_context (nano::frontier_scan_config config_a = {}) :
		config{ config_a },
		engine{ config, stats },
		peer_status{ [] (std::span<nano::account const>) {
			return nano::bootstrap::peer_probe_status::available;
		} }
	{
	}

	nano::bootstrap::frontier_scan_engine::probes probes ()
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

	std::shared_ptr<nano::bootstrap::frontier_round> next ()
	{
		auto round = engine.next_round (now, probes ());
		release_assert (round != nullptr);
		return round;
	}

	void commit (std::shared_ptr<nano::bootstrap::frontier_round> const & round, nano::account node_id, nano::bootstrap::id_t id)
	{
		round->reserve_sample (node_id, id, now);
		live_tags.insert (id);
	}

	bool feed (nano::bootstrap::id_t id, nano::account const & start, std::deque<std::pair<nano::account, nano::block_hash>> const & response)
	{
		live_tags.erase (id);
		return engine.process (id, start, response);
	}
};
}

TEST (bootstrap_frontier_round, quorum_done)
{
	nano::frontier_scan_config config;
	config.consideration_count = 3;
	config.candidates = 8;
	nano::bootstrap::frontier_round round{ config, nano::account{ 1 }, nano::account{ 100 } };

	round.feed (frontiers ({ 2, 3 }));
	round.feed (frontiers ({ 2, 4 }));
	ASSERT_FALSE (round.done ());

	round.feed (frontiers ({ 3, 5 }));
	ASSERT_TRUE (round.done ());
	ASSERT_EQ (round.conclude (), nano::account{ 5 });
}

TEST (bootstrap_frontier_round, empty_range)
{
	nano::frontier_scan_config config;
	config.consideration_count = 2;
	nano::bootstrap::frontier_round round{ config, nano::account{ 1 }, nano::account{ 100 } };

	round.feed ({});
	round.feed ({});
	round.feed ({});
	ASSERT_FALSE (round.empty_range ());

	round.feed ({});
	ASSERT_TRUE (round.empty_range ());
	ASSERT_EQ (round.conclude (), nano::account{ 100 });
}

TEST (bootstrap_frontier_round, trim_keeps_smallest_candidates)
{
	nano::frontier_scan_config config;
	config.consideration_count = 1;
	config.candidates = 3;
	nano::bootstrap::frontier_round round{ config, nano::account{ 1 }, nano::account{ 100 } };

	round.feed (frontiers ({ 2, 5, 9, 4 }));

	ASSERT_EQ (round.candidate_count (), 3);
	ASSERT_EQ (round.conclude (), nano::account{ 5 });
}

TEST (bootstrap_frontier_scan, eager_fanout_tracks_distinct_peers)
{
	nano::frontier_scan_config config;
	config.head_parallelism = 1;
	config.consideration_count = 3;
	test_context ctx{ config };

	auto first = ctx.next ();
	ASSERT_EQ (first->position (), nano::account{ 1 });
	ASSERT_TRUE (first->exclude ().empty ());
	ctx.commit (first, nano::account{ 10 }, 1);

	auto second = ctx.next ();
	ASSERT_EQ (second->position (), first->position ());
	ASSERT_EQ (second->exclude ().size (), 1);
	ASSERT_EQ (second->exclude ()[0], nano::account{ 10 });
	ctx.commit (second, nano::account{ 11 }, 2);

	auto third = ctx.next ();
	ASSERT_EQ (third->exclude ().size (), 2);
	ASSERT_EQ (third->exclude ()[0], nano::account{ 10 });
	ASSERT_EQ (third->exclude ()[1], nano::account{ 11 });
	ctx.commit (third, nano::account{ 12 }, 3);

	ASSERT_EQ (ctx.engine.next_round (ctx.now, ctx.probes ()), nullptr);
}

TEST (bootstrap_frontier_scan, next_round_reuses_unreserved_round)
{
	nano::frontier_scan_config config;
	config.head_parallelism = 2;
	test_context ctx{ config };

	auto first = ctx.next ();
	auto second = ctx.next ();

	ASSERT_EQ (second, first);
	ASSERT_EQ (second->position (), first->position ());
}

TEST (bootstrap_frontier_scan, quorum_settles_clean)
{
	nano::frontier_scan_config config;
	config.head_parallelism = 1;
	config.consideration_count = 2;
	test_context ctx{ config };

	auto first = ctx.next ();
	ctx.commit (first, nano::account{ 10 }, 1);
	auto second = ctx.next ();
	ctx.commit (second, nano::account{ 11 }, 2);

	ASSERT_TRUE (ctx.feed (1, first->position (), frontiers ({ 2 })));
	ASSERT_TRUE (ctx.feed (2, first->position (), frontiers ({ 3 })));
	ctx.engine.settle (ctx.now, ctx.probes ());

	auto next = ctx.next ();
	ASSERT_EQ (next->position (), nano::account{ 3 });
}

TEST (bootstrap_frontier_scan, exhausted_partial_concludes_after_response)
{
	nano::frontier_scan_config config;
	config.head_parallelism = 1;
	config.consideration_count = 4;
	test_context ctx{ config };

	auto first = ctx.next ();
	ctx.commit (first, nano::account{ 10 }, 1);
	ctx.peer_status = [] (std::span<nano::account const> exclude) {
		return exclude.empty () ? nano::bootstrap::peer_probe_status::available : nano::bootstrap::peer_probe_status::none;
	};

	ASSERT_TRUE (ctx.feed (1, first->position (), frontiers ({ 2 })));
	ctx.engine.settle (ctx.now, ctx.probes ());

	ASSERT_EQ (ctx.engine.next_round (ctx.now, ctx.probes ()), nullptr);
	ctx.now += config.cooldown;
	auto next = ctx.next ();
	ASSERT_EQ (next->position (), nano::account{ 2 });
}

TEST (bootstrap_frontier_scan, exhausted_empty_wraps_with_partial_stat)
{
	nano::frontier_scan_config config;
	config.head_parallelism = 1;
	config.consideration_count = 4;
	test_context ctx{ config };

	auto first = ctx.next ();
	ctx.commit (first, nano::account{ 10 }, 1);
	ctx.peer_status = [] (std::span<nano::account const> exclude) {
		return exclude.empty () ? nano::bootstrap::peer_probe_status::available : nano::bootstrap::peer_probe_status::none;
	};

	ASSERT_TRUE (ctx.feed (1, first->position (), {}));
	ctx.engine.settle (ctx.now, ctx.probes ());

	ctx.now += config.cooldown;
	auto next = ctx.next ();
	ASSERT_EQ (next->position (), nano::account{ 1 });
}

TEST (bootstrap_frontier_scan, ride_out_inflight_before_done_none)
{
	nano::frontier_scan_config config;
	config.head_parallelism = 1;
	config.consideration_count = 4;
	test_context ctx{ config };

	auto first = ctx.next ();
	ctx.commit (first, nano::account{ 10 }, 1);
	ctx.peer_status = [] (std::span<nano::account const> exclude) {
		return exclude.empty () ? nano::bootstrap::peer_probe_status::available : nano::bootstrap::peer_probe_status::none;
	};

	ctx.engine.settle (ctx.now, ctx.probes ());
	ASSERT_EQ (ctx.engine.next_round (ctx.now, ctx.probes ()), nullptr);

	ctx.live_tags.erase (1);
	ctx.engine.settle (ctx.now, ctx.probes ());
	ctx.now += config.cooldown;
	auto retry = ctx.next ();
	ASSERT_EQ (retry->position (), first->position ());
}

TEST (bootstrap_frontier_scan, straggler_rejected_after_settlement)
{
	nano::frontier_scan_config config;
	config.head_parallelism = 1;
	config.consideration_count = 2;
	test_context ctx{ config };

	auto first = ctx.next ();
	ctx.commit (first, nano::account{ 10 }, 1);
	auto second = ctx.next ();
	ctx.commit (second, nano::account{ 11 }, 2);

	ctx.now += config.cooldown;
	auto third = ctx.next ();
	ctx.commit (third, nano::account{ 12 }, 3);

	ASSERT_TRUE (ctx.feed (1, first->position (), frontiers ({ 2 })));
	ASSERT_TRUE (ctx.feed (2, first->position (), frontiers ({ 3 })));
	ctx.engine.settle (ctx.now, ctx.probes ());

	ASSERT_FALSE (ctx.feed (3, first->position (), frontiers ({ 4 })));
	auto next = ctx.next ();
	ASSERT_EQ (next->position (), nano::account{ 3 });
}

TEST (bootstrap_frontier_scan, retry_round_ignores_predecessor_id)
{
	nano::frontier_scan_config config;
	config.head_parallelism = 1;
	config.consideration_count = 4;
	test_context ctx{ config };

	auto first = ctx.next ();
	ctx.commit (first, nano::account{ 10 }, 1);
	ctx.peer_status = [] (std::span<nano::account const> exclude) {
		return exclude.empty () ? nano::bootstrap::peer_probe_status::available : nano::bootstrap::peer_probe_status::none;
	};
	ctx.live_tags.erase (1);
	ctx.engine.settle (ctx.now, ctx.probes ());

	ctx.now += config.cooldown;
	auto retry = ctx.next ();
	ctx.commit (retry, nano::account{ 10 }, 2);

	ASSERT_FALSE (ctx.engine.process (1, first->position (), frontiers ({ 2 })));
	ASSERT_TRUE (ctx.feed (2, retry->position (), frontiers ({ 2 })));
}

TEST (bootstrap_frontier_scan, busy_head_is_skipped)
{
	nano::frontier_scan_config config;
	config.head_parallelism = 2;
	config.consideration_count = 4;
	test_context ctx{ config };

	auto first = ctx.next ();
	ctx.commit (first, nano::account{ 10 }, 1);
	ctx.peer_status = [] (std::span<nano::account const> exclude) {
		return exclude.empty () ? nano::bootstrap::peer_probe_status::available : nano::bootstrap::peer_probe_status::busy;
	};

	auto next = ctx.next ();
	ASSERT_NE (next, first);
	ASSERT_TRUE (next->exclude ().empty ());
}

TEST (bootstrap_frontier_scan, erase_sample_after_reset_is_noop)
{
	nano::frontier_scan_config config;
	config.head_parallelism = 1;
	test_context ctx{ config };

	auto first = ctx.next ();
	ctx.commit (first, nano::account{ 10 }, 1);
	ctx.engine.reset ();

	ctx.engine.erase_sample (1, first->position ());
	auto next = ctx.next ();
	ASSERT_EQ (next->position (), nano::account{ 1 });
}
