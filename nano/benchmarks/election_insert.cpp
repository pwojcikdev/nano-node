/*
 * Benchmarks of active_elections::insert, the entry point every election scheduler goes through.
 * A scheduler hands insert a block it read earlier, so insert first compares it with the ledger's current block for its root.
 * Every benchmark reports the time and the heap allocations per operation, so a change can be judged on both.
 *
 * The fixture is a dev network node with `block_count` unconfirmed blocks on independent accounts.
 * Each block follows an open block, so its check goes through the successor table, and `ballast_entries` random entries give that table the depth of a large ledger.
 *
 * Scenarios, named after what insert does with the block:
 * - started: the root has no election, one starts
 * - paced: as started, one insert per `insert_pace` so background workers go idle in between, the way they do at the rates a node starts elections
 * - running: the root has an election holding the block, nothing changes
 * - stale: the ledger no longer holds the block, it is refused
 *
 * read_transaction and successor_lookup time the two halves of the check on their own, successor_lookup_cold repeats the lookup on keys spread over the whole table.
 * priority_activate times the ledger reads a block goes through before it reaches insert.
 *
 * The reported time is the time one thread spends on one operation.
 * Record a run with --benchmark_out=<file> --benchmark_out_format=json and compare two of them with submodules/benchmark/tools/compare.py
 */

#include <nano/benchmarks/allocations.hpp>
#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/config.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/backlog_scan.hpp>
#include <nano/node/election_behavior.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/scheduler/component.hpp>
#include <nano/node/scheduler/hinted.hpp>
#include <nano/node/scheduler/optimistic.hpp>
#include <nano/node/scheduler/priority.hpp>
#include <nano/node/vote_processor.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/ledger/successor.hpp>
#include <nano/store/ledger_store.hpp>
#include <nano/test_common/chains.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

#include <benchmark/benchmark.h>

namespace
{
constexpr size_t block_count = 1024;
constexpr size_t ballast_entries = 5 * 1024 * 1024;
constexpr size_t ballast_batch = 50 * 1024;
constexpr size_t ballast_sampling = 8; // One in this many ballast keys is kept for the cold lookups
constexpr auto insert_pace = std::chrono::microseconds{ 100 };

// The benchmarks' main leaves the network alone, a node must never start on another one from here
struct dev_network
{
	dev_network ()
	{
		nano::force_nano_dev_network ();
	}
};

nano::block_hash random_hash ()
{
	nano::block_hash hash;
	nano::random_pool::generate_block (hash.bytes.data (), hash.bytes.size ());
	return hash;
}

class insert_fixture
{
public:
	insert_fixture ()
	{
		auto config = system.default_config ();
		// Elections are started by hand, anything else starting them would make runs differ
		config.hinted_scheduler->enable = false;
		config.optimistic_scheduler->enable = false;
		config.priority_scheduler->enable = false;
		config.backlog_scan->enable = false;
		config.vote_cache_processor->enable = false;
		node = system.add_node (config);

		blocks = setup_blocks (block_count);
		stale_blocks = setup_blocks (block_count);
		for (auto const & block : blocks)
		{
			roots.push_back (block->qualified_root ());
		}

		// Rolled back blocks keep their sideband, which is all a scheduler's stale entry has to offer
		{
			auto transaction = node->ledger.tx_begin_write ();
			for (auto const & block : stale_blocks)
			{
				bool const error = node->ledger.rollback (transaction, block->hash ());
				release_assert (!error);
			}
		}

		fill_ballast (ballast_entries);
	}

	// Gives every block a fresh election
	void restart_elections ()
	{
		node->active.clear ();
		for (auto const & block : blocks)
		{
			auto const result = node->active.insert (block, nano::election_behavior::priority);
			release_assert (result.outcome == nano::active_elections::insert_outcome::inserted);
		}
	}

private:
	// One block on each of `count` accounts, its open block confirmed so the block is next in line for an election
	nano::block_list_t setup_blocks (size_t count)
	{
		nano::block_list_t result;
		for (auto const & chain : nano::test::setup_chains (system, *node, static_cast<int> (count), 1, nano::dev::genesis_key, /* do not confirm */ false))
		{
			nano::test::confirm (node->ledger, chain.second.front ());
			result.push_back (chain.second.back ());
		}
		return result;
	}

	// Keys and values have the distribution of real successor entries, nothing ever looks these up
	void fill_ballast (size_t count)
	{
		for (size_t written = 0; written < count; written += ballast_batch)
		{
			auto transaction = node->ledger.tx_begin_write ();
			for (size_t i = 0; i < ballast_batch; ++i)
			{
				auto const key = random_hash ();
				node->ledger.store.successor.put (transaction, key, random_hash ());
				if (i % ballast_sampling == 0)
				{
					ballast_keys.push_back (key);
				}
			}
		}
	}

public:
	dev_network network;
	nano::test::system system;
	std::shared_ptr<nano::node> node;
	nano::block_list_t blocks;
	nano::block_list_t stale_blocks;
	std::vector<nano::qualified_root> roots;
	std::vector<nano::block_hash> ballast_keys;
};

// Built on first use, building it takes seconds
insert_fixture & shared_fixture ()
{
	static insert_fixture instance;
	return instance;
}

// Every thread starts at its own part of the list, so threads rarely meet on an election
size_t first_index (benchmark::State const & state)
{
	return state.thread_index () * block_count / state.threads ();
}
}

static void BM_insert_started (benchmark::State & state)
{
	auto & fixture = shared_fixture ();
	fixture.node->active.clear ();
	nano::benchmarks::allocation_counter allocations{ "insert" };
	size_t index = 0;
	for (auto _ : state)
	{
		if (index == fixture.blocks.size ())
		{
			state.PauseTiming ();
			fixture.node->active.clear ();
			state.ResumeTiming ();
			index = 0;
		}
		auto const result = fixture.node->active.insert (fixture.blocks[index++], nano::election_behavior::priority);
		release_assert (result.outcome == nano::active_elections::insert_outcome::inserted);
	}
	allocations.report (state);
}

// Reports only the time spent in insert, the pause in between stays out of it
static void BM_insert_paced (benchmark::State & state)
{
	auto & fixture = shared_fixture ();
	fixture.node->active.clear ();
	std::vector<double> samples;
	samples.reserve (1 << 18);
	nano::benchmarks::allocation_counter allocations{ "insert" };
	size_t index = 0;
	for (auto _ : state)
	{
		if (index == fixture.blocks.size ())
		{
			fixture.node->active.clear ();
			index = 0;
		}
		auto const resume = std::chrono::steady_clock::now () + insert_pace;
		while (std::chrono::steady_clock::now () < resume)
		{
		}
		auto const start = std::chrono::steady_clock::now ();
		auto const result = fixture.node->active.insert (fixture.blocks[index++], nano::election_behavior::priority);
		auto const stop = std::chrono::steady_clock::now ();
		state.SetIterationTime (std::chrono::duration<double> (stop - start).count ());
		samples.push_back (std::chrono::duration<double, std::nano> (stop - start).count ());
		release_assert (result.outcome == nano::active_elections::insert_outcome::inserted);
	}
	allocations.report (state);

	// One preempted insert moves the mean of a paced run, the median stays put
	std::nth_element (samples.begin (), samples.begin () + samples.size () / 2, samples.end ());
	state.counters["p50_ns"] = samples[samples.size () / 2];
}

static void BM_insert_running (benchmark::State & state)
{
	auto & fixture = shared_fixture ();
	if (state.thread_index () == 0)
	{
		fixture.restart_elections ();
	}
	nano::benchmarks::allocation_counter allocations{ "insert" };
	size_t index = first_index (state);
	for (auto _ : state)
	{
		auto const result = fixture.node->active.insert (fixture.blocks[index++ % fixture.blocks.size ()], nano::election_behavior::priority);
		release_assert (result.outcome == nano::active_elections::insert_outcome::existing);
	}
	allocations.report (state);
}

static void BM_insert_stale (benchmark::State & state)
{
	auto & fixture = shared_fixture ();
	if (state.thread_index () == 0)
	{
		fixture.node->active.clear ();
	}
	nano::benchmarks::allocation_counter allocations{ "insert" };
	size_t index = first_index (state);
	for (auto _ : state)
	{
		auto const result = fixture.node->active.insert (fixture.stale_blocks[index++ % fixture.stale_blocks.size ()], nano::election_behavior::priority);
		release_assert (result.outcome == nano::active_elections::insert_outcome::stale);
	}
	allocations.report (state);
}

static void BM_read_transaction (benchmark::State & state)
{
	auto & fixture = shared_fixture ();
	if (state.thread_index () == 0)
	{
		fixture.node->active.clear ();
	}
	nano::benchmarks::allocation_counter allocations{ "transaction" };
	for (auto _ : state)
	{
		auto const transaction = fixture.node->ledger.tx_begin_read ();
		benchmark::DoNotOptimize (&transaction);
	}
	allocations.report (state);
}

static void BM_successor_lookup (benchmark::State & state)
{
	auto & fixture = shared_fixture ();
	if (state.thread_index () == 0)
	{
		fixture.node->active.clear ();
	}
	auto const transaction = fixture.node->ledger.tx_begin_read ();
	nano::benchmarks::allocation_counter allocations{ "lookup" };
	size_t index = first_index (state);
	for (auto _ : state)
	{
		auto const successor = fixture.node->ledger.any.block_successor (transaction, fixture.roots[index++ % fixture.roots.size ()]);
		benchmark::DoNotOptimize (successor);
	}
	allocations.report (state);
}

static void BM_successor_lookup_cold (benchmark::State & state)
{
	auto & fixture = shared_fixture ();
	fixture.node->active.clear ();
	auto const transaction = fixture.node->ledger.tx_begin_read ();
	nano::benchmarks::allocation_counter allocations{ "lookup" };
	size_t index = 0;
	for (auto _ : state)
	{
		auto const successor = fixture.node->ledger.store.successor.get (transaction, fixture.ballast_keys[index++ % fixture.ballast_keys.size ()]);
		benchmark::DoNotOptimize (successor);
	}
	allocations.report (state);
}

// The scheduler pool drops the block as a duplicate after the first round, what remains is the reads
static void BM_priority_activate (benchmark::State & state)
{
	auto & fixture = shared_fixture ();
	fixture.node->active.clear ();
	auto const transaction = fixture.node->ledger.tx_begin_read ();
	nano::benchmarks::allocation_counter allocations{ "activate" };
	size_t index = 0;
	for (auto _ : state)
	{
		bool const activated = fixture.node->scheduler.priority.activate (transaction, fixture.blocks[index++ % fixture.blocks.size ()]->account ());
		benchmark::DoNotOptimize (activated);
	}
	allocations.report (state);
}

BENCHMARK (BM_insert_started)->UseRealTime ();
BENCHMARK (BM_insert_paced)->MinTime (0.15)->UseManualTime ();
BENCHMARK (BM_insert_running)->Threads (1)->Threads (4)->UseRealTime ();
BENCHMARK (BM_insert_stale)->Threads (1)->Threads (4)->UseRealTime ();
BENCHMARK (BM_read_transaction)->Threads (1)->Threads (4)->UseRealTime ();
BENCHMARK (BM_successor_lookup)->Threads (1)->Threads (4)->UseRealTime ();
BENCHMARK (BM_successor_lookup_cold)->UseRealTime ();
BENCHMARK (BM_priority_activate)->UseRealTime ();
