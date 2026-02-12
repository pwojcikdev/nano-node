#include <nano/lib/block_type.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stream.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/topology.hpp>
#include <nano/store/ledger/version.hpp>
#include <nano/store/ledger_store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
std::vector<uint8_t> serialize_block_with_sideband (nano::block const & block)
{
	std::vector<uint8_t> data;
	nano::vectorstream stream{ data };
	nano::serialize_block (stream, block);
	block.sideband ().serialize (stream, block.type ());
	return data;
}

struct topology_update
{
	nano::block_hash hash{ 0 };
	uint64_t topo_index{ 0 };
};

struct dfs_frame
{
	nano::block_hash hash{ 0 };
	std::array<nano::block_hash, 2> dependencies{ 0, 0 };
	size_t next_dependency{ 0 };
	bool initialized{ false };
};

struct dfs_result
{
	std::vector<topology_update> resolved;
	bool overflowed{ false };
	bool truncated{ false };
};

// Performs bounded iterative DFS to compute topo_index for a block and all its unresolved dependencies.
// Uses a read-only transaction and collects only hash/index pairs to keep memory bounded.
// If stack depth or local result budget is exceeded, returns partial results.
dfs_result compute_topo_bounded (
nano::store::ledger_store & store,
nano::store::transaction const & tx,
nano::block_hash const & start_hash,
size_t max_depth,
size_t max_resolved)
{
	dfs_result result;
	result.resolved.reserve (std::min<size_t> (max_resolved, 512));

	std::unordered_map<nano::block_hash, uint64_t> local_cache;
	local_cache.reserve (2048);

	std::vector<dfs_frame> stack;
	stack.reserve (std::min<size_t> (max_depth, 4096));
	stack.push_back ({ start_hash });

	while (!stack.empty ())
	{
		auto & current = stack.back ();

		// Already resolved in this traversal
		if (auto it = local_cache.find (current.hash); it != local_cache.end ())
		{
			stack.pop_back ();
			continue;
		}

		// First visit: load the block and check if already in topology table
		if (!current.initialized)
		{
			auto current_block = store.block.get (tx, current.hash);
			release_assert (current_block != nullptr, "missing block while rebuilding topology index", current.hash.to_string ());

			auto const persisted_index = current_block->sideband ().topo_index;
			if (store.topology.exists (tx, persisted_index, current.hash))
			{
				// Already processed (from this or a previous run)
				local_cache.emplace (current.hash, persisted_index);
				stack.pop_back ();
				continue;
			}

			current.dependencies = current_block->block_dependencies ();
			current.next_dependency = 0;
			current.initialized = true;
		}

		// Try to resolve unvisited dependencies
		bool pushed = false;
		while (current.next_dependency < current.dependencies.size ())
		{
			auto const dependency = current.dependencies[current.next_dependency++];
			if (dependency.is_zero ())
			{
				continue;
			}

			if (local_cache.count (dependency))
			{
				continue;
			}

			// Check if dependency exists and is already resolved
			auto dependency_block = store.block.get (tx, dependency);
			if (dependency_block == nullptr)
			{
				// Missing dependency (e.g. pruned source) treated as external
				continue;
			}

			auto const dependency_index = dependency_block->sideband ().topo_index;
			if (store.topology.exists (tx, dependency_index, dependency))
			{
				local_cache.emplace (dependency, dependency_index);
				continue;
			}

			// Unresolved dependency: check stack depth before pushing
			if (stack.size () >= max_depth)
			{
				result.overflowed = true;
				return result;
			}

			stack.push_back ({ dependency });
			pushed = true;
			break;
		}

		if (pushed)
		{
			continue;
		}

		// All dependencies resolved: compute topo_index = max(dep indices) + 1
		uint64_t current_index{ 0 };
		for (auto const & dependency : current.dependencies)
		{
			if (dependency.is_zero ())
			{
				continue;
			}
			if (auto it = local_cache.find (dependency); it != local_cache.end ())
			{
				current_index = std::max (current_index, it->second + 1);
			}
		}

		if (result.resolved.size () >= max_resolved)
		{
			result.truncated = true;
			return result;
		}
		result.resolved.push_back ({ current.hash, current_index });

		local_cache.emplace (current.hash, current_index);
		stack.pop_back ();
	}

	return result;
}
}

namespace nano::store
{
// Builds a topological ordering index for every block in the ledger.
// Each block is assigned an index that is strictly greater than all of its dependencies,
// allowing blocks to be iterated in dependency-respecting order without recomputing at runtime.
// The index is stored both in each block's sideband data and in a new `topology` table.
//
// The upgrade is restartable: if interrupted, previously computed entries in the topology
// table are preserved and skipped on the next run.
void ledger_store::upgrade_v24_to_v25 ()
{
	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v24 to v25...");

	backend.open (schema_v25, nano::store::open_mode::read_write);
	{
		auto version_tx = backend.tx_begin_read ();
		auto const current_version = backend.get_version (version_tx);
		release_assert (current_version == 24, "unexpected version during upgrade", std::to_string (current_version));

		uint64_t total_blocks{ 0 };
		uint64_t already_processed{ 0 };
		{
			auto read_tx = backend.tx_begin_read ();
			total_blocks = block.count (read_tx);
			already_processed = topology.count (read_tx);
		}

		if (already_processed > 0)
		{
			logger.info (nano::log::type::ledger_upgrade, "Resuming topology index rebuild: {} / {} blocks already processed", already_processed, total_blocks);
		}
		logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: {} blocks total, {} remaining", total_blocks, total_blocks - already_processed);

		// Memory bounds:
		// - DFS stack and per-traversal cache are bounded by max_stack_depth.
		// - DFS output per starting block is bounded by max_dfs_results.
		// - Global per-pass candidates are bounded by max_results_per_pass.
		// This keeps the upgrade restartable while capping memory use and preserving parallel traversal.
		size_t const max_stack_depth = nano::is_dev_run () ? 1024 : 65'536;
		size_t const max_dfs_results = nano::is_dev_run () ? 256 : 4096;
		size_t const max_results_per_pass = nano::is_dev_run () ? 1024 : 250'000;
		size_t const local_flush_size = 512;
		auto const write_batch_size = nano::is_dev_run () ? 2 : 5000;
		auto const write_progress_step = std::max<uint64_t> (1, total_blocks / 100); // 1%
		auto const compute_progress_step = std::max<uint64_t> (1, total_blocks / 20); // 5%

		logger.info (nano::log::type::ledger_upgrade,
		"Topology rebuild settings: max_stack_depth={}, max_dfs_results={}, max_results_per_pass={}, write_batch_size={}",
		max_stack_depth,
		max_dfs_results,
		max_results_per_pass,
		write_batch_size);

		for (uint64_t pass = 1; already_processed < total_blocks; ++pass)
		{
			logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: pass {} started (stage=compute, progress={} / {})",
			pass, already_processed, total_blocks);

			// Stage 1: Parallel bounded DFS compute phase
			std::vector<topology_update> pass_results;
			pass_results.reserve (max_results_per_pass);
			std::unordered_set<nano::block_hash> pass_seen;
			pass_seen.reserve (max_results_per_pass);

			std::mutex results_mutex;
			std::atomic<bool> pass_full{ false };
			std::atomic<uint64_t> iterated{ 0 };
			std::atomic<uint64_t> accepted{ 0 };
			std::atomic<uint64_t> overflowed{ 0 };
			std::atomic<uint64_t> truncated{ 0 };
			std::atomic<uint64_t> next_compute_log{ compute_progress_step };

			auto flush_local_results = [&] (std::vector<topology_update> & local_results) {
				if (local_results.empty ())
				{
					return;
				}

				std::lock_guard lock{ results_mutex };
				for (auto & item : local_results)
				{
					if (pass_results.size () >= max_results_per_pass)
					{
						pass_full.store (true, std::memory_order_relaxed);
						break;
					}
					if (!pass_seen.insert (item.hash).second)
					{
						continue;
					}
					pass_results.push_back (item);
					accepted.fetch_add (1, std::memory_order_relaxed);
				}
				local_results.clear ();
			};

			block.for_each_par ([&] (nano::store::read_transaction const & read_tx, auto i, auto n) {
				std::vector<topology_update> local_results;
				local_results.reserve (local_flush_size);

				for (; i != n && !pass_full.load (std::memory_order_relaxed); ++i)
				{
					auto const iterated_now = iterated.fetch_add (1, std::memory_order_relaxed) + 1;
					auto log_target = next_compute_log.load (std::memory_order_relaxed);
					if (iterated_now >= log_target && next_compute_log.compare_exchange_strong (log_target, log_target + compute_progress_step, std::memory_order_relaxed))
					{
						logger.info (nano::log::type::ledger_upgrade,
						"Rebuilding topology index: pass {} compute progress: scanned {} / {} blocks, accepted {} candidates",
						pass,
						iterated_now,
						total_blocks,
						accepted.load (std::memory_order_relaxed));
					}

					auto const & hash = i->first;
					auto const & bws = i->second;
					auto const & blk = bws.block;

					if (topology.exists (read_tx, blk->sideband ().topo_index, hash))
					{
						continue; // Already processed
					}

					auto dfs = compute_topo_bounded (*this, read_tx, hash, max_stack_depth, max_dfs_results);
					if (dfs.overflowed)
					{
						overflowed.fetch_add (1, std::memory_order_relaxed);
					}
					if (dfs.truncated)
					{
						truncated.fetch_add (1, std::memory_order_relaxed);
					}
					if (!dfs.resolved.empty ())
					{
						for (auto & item : dfs.resolved)
						{
							local_results.push_back (std::move (item));
							if (local_results.size () >= local_flush_size)
							{
								flush_local_results (local_results);
							}
							if (pass_full.load (std::memory_order_relaxed))
							{
								break;
							}
						}
					}
				}

				flush_local_results (local_results);
			});

			logger.info (nano::log::type::ledger_upgrade,
			"Rebuilding topology index: pass {} compute complete, scanned {} blocks, accepted {} candidates, overflowed={}, truncated={}, pass_limited={}",
			pass,
			iterated.load (std::memory_order_relaxed),
			accepted.load (std::memory_order_relaxed),
			overflowed.load (std::memory_order_relaxed),
			truncated.load (std::memory_order_relaxed),
			pass_full.load (std::memory_order_relaxed));

			// Stage 2: Sequential write phase
			logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: pass {} started (stage=write, candidates={})", pass, pass_results.size ());

			std::sort (pass_results.begin (), pass_results.end (), [] (auto const & lhs, auto const & rhs) {
				if (lhs.topo_index != rhs.topo_index)
				{
					return lhs.topo_index < rhs.topo_index;
				}
				return lhs.hash < rhs.hash;
			});

			auto write_tx = backend.tx_begin_write ();
			uint64_t written{ 0 };
			uint64_t skipped_existing{ 0 };
			uint64_t last_log_written{ already_processed };

			for (auto const & item : pass_results)
			{
				// Skip duplicates (same block resolved by multiple threads)
				if (topology.exists (write_tx, item.topo_index, item.hash))
				{
					++skipped_existing;
					continue;
				}

				auto block_ptr = block.get (write_tx, item.hash);
				release_assert (block_ptr != nullptr, "missing block while writing topology index", item.hash.to_string ());

				auto sideband = block_ptr->sideband ();
				sideband.topo_index = item.topo_index;
				block_ptr->sideband_set (sideband);

				block.raw_put (write_tx, serialize_block_with_sideband (*block_ptr), item.hash);
				topology.put (write_tx, item.topo_index, item.hash);
				++written;

				// Log progress at regular intervals
				auto const total_written = already_processed + written;
				if (total_written - last_log_written >= write_progress_step)
				{
					double const percentage = total_blocks == 0 ? 100.0 : (100.0 * static_cast<double> (total_written) / static_cast<double> (total_blocks));
					logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: pass {} write progress: {} / {} blocks ({:.2f}%)",
					pass, total_written, total_blocks, percentage);
					last_log_written = total_written;
				}

				if (written % write_batch_size == 0)
				{
					write_tx.refresh ();
				}
			}

			auto const total_done = topology.count (write_tx);
			double const percentage = total_blocks == 0 ? 100.0 : (100.0 * static_cast<double> (total_done) / static_cast<double> (total_blocks));
			logger.info (nano::log::type::ledger_upgrade,
			"Rebuilding topology index: pass {} complete (stage=write), wrote {}, skipped {}, total {} / {} ({:.2f}%)",
			pass,
			written,
			skipped_existing,
			total_done,
			total_blocks,
			percentage);

			release_assert (total_done > already_processed, "no progress made during topology rebuild pass", std::to_string (pass));

			already_processed = total_done;
		}

		auto write_tx = backend.tx_begin_write ();
		release_assert (topology.count (write_tx) == total_blocks, "topology table count mismatch after rebuild",
		std::to_string (topology.count (write_tx)) + " != " + std::to_string (total_blocks));
		version.put (write_tx, 25);
	}
	backend.close ();

	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v24 to v25 completed");
}
}
