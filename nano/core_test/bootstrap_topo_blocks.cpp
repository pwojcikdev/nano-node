#include <nano/lib/logging.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/bootstrap/topo_blocks.hpp>
#include <nano/secure/common.hpp>

#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono_literals;

namespace
{
struct test_context
{
	nano::logger & logger;
	nano::stats stats;
	nano::topo_scan_config config;
	nano::bootstrap::topo_blocks blocks;

	test_context () :
		logger{ nano::default_logger () },
		stats{ logger },
		blocks{ config, stats, logger }
	{
		config.fetch_cooldown = 2s;
		config.max_fetch_attempts = 10;
	}
};

nano::topo_key make_topo_key (uint64_t height, uint64_t hash)
{
	return nano::topo_key{ height, nano::block_hash{ hash } };
}
}

TEST (bootstrap_topo_blocks, next_excludes_sampled_peer)
{
	test_context ctx;
	auto const key = make_topo_key (100, 1);
	auto const peer1 = nano::account{ 1 };
	auto const now = std::chrono::steady_clock::now ();

	ctx.blocks.add ({ key });

	auto req = ctx.blocks.next (1, now);
	ASSERT_TRUE (req);
	ASSERT_EQ (req->hashes.size (), 1);
	ASSERT_EQ (req->hashes.front (), key.hash);
	ASSERT_TRUE (req->exclude.empty ());

	ASSERT_TRUE (ctx.blocks.dispatch (*req, 1, peer1, now));
	ctx.blocks.process (1, {});

	req = ctx.blocks.next (1, now + ctx.config.fetch_cooldown);
	ASSERT_TRUE (req);
	ASSERT_EQ (req->hashes.size (), 1);
	ASSERT_EQ (req->hashes.front (), key.hash);
	ASSERT_EQ (req->exclude.size (), 1);
	ASSERT_EQ (req->exclude.front (), peer1);
}

TEST (bootstrap_topo_blocks, exhausted_resets_sampled_peers)
{
	test_context ctx;
	auto const key = make_topo_key (100, 1);
	auto const peer1 = nano::account{ 1 };
	auto const now = std::chrono::steady_clock::now ();

	ctx.blocks.add ({ key });

	auto req = ctx.blocks.next (1, now);
	ASSERT_TRUE (req);
	ASSERT_TRUE (ctx.blocks.dispatch (*req, 1, peer1, now));
	ctx.blocks.process (1, {});

	req = ctx.blocks.next (1, now + ctx.config.fetch_cooldown);
	ASSERT_TRUE (req);
	ASSERT_EQ (req->exclude.size (), 1);

	ctx.blocks.exhausted (*req);

	req = ctx.blocks.next (1, now + ctx.config.fetch_cooldown);
	ASSERT_TRUE (req);
	ASSERT_TRUE (req->exclude.empty ());
}

TEST (bootstrap_topo_blocks, cancel_rearms_without_resampling_peer)
{
	test_context ctx;
	auto const key = make_topo_key (100, 1);
	auto const peer1 = nano::account{ 1 };
	auto const now = std::chrono::steady_clock::now ();

	ctx.blocks.add ({ key });

	auto req = ctx.blocks.next (1, now);
	ASSERT_TRUE (req);
	ASSERT_TRUE (ctx.blocks.dispatch (*req, 1, peer1, now));

	ctx.blocks.cancel (1);

	req = ctx.blocks.next (1, now);
	ASSERT_TRUE (req);
	ASSERT_EQ (req->hashes.size (), 1);
	ASSERT_EQ (req->hashes.front (), key.hash);
	ASSERT_EQ (req->exclude.size (), 1);
	ASSERT_EQ (req->exclude.front (), peer1);
}
