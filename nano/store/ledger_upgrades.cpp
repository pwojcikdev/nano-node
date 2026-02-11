#include <nano/lib/block_type.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stream.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/topology.hpp>
#include <nano/store/ledger/version.hpp>
#include <nano/store/ledger_store.hpp>

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
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

struct computed_block
{
	nano::block_hash hash{ 0 };
	uint64_t topo_index{ 0 };
	std::vector<uint8_t> serialized;
};

struct dfs_frame
{
	nano::block_hash hash{ 0 };
	std::array<nano::block_hash, 2> dependencies{ 0, 0 };
	size_t next_dependency{ 0 };
	std::shared_ptr<nano::block> block{};
};

struct dfs_result
{
	std::vector<computed_block> resolved;
	bool overflowed{ false };
};

// Performs bounded iterative DFS to compute topo_index for a block and all its unresolved dependencies.
// Uses a read-only transaction; resolved blocks are collected in the result rather than written immediately.
// If the stack depth exceeds max_depth, returns with overflowed=true and partial results.
dfs_result compute_topo_bounded (
nano::store::ledger_store & store,
nano::store::transaction const & tx,
nano::block_hash const & start_hash,
size_t max_depth)
{
	dfs_result result;

	std::unordered_map<nano::block_hash, uint64_t> local_cache;
	local_cache.reserve (2048);

	std::vector<dfs_frame> stack;
	stack.reserve (2048);
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
		if (!current.block)
		{
			current.block = store.block.get (tx, current.hash);
			release_assert (current.block != nullptr, "missing block while rebuilding topology index", current.hash.to_string ());

			auto const persisted_index = current.block->sideband ().topo_index;
			if (store.topology.exists (tx, persisted_index, current.hash))
			{
				// Already processed (from this or a previous run)
				local_cache.emplace (current.hash, persisted_index);
				stack.pop_back ();
				continue;
			}

			current.dependencies = current.block->block_dependencies ();
			current.next_dependency = 0;
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

		// Collect the resolved block for later writing
		auto sideband = current.block->sideband ();
		sideband.topo_index = current_index;
		current.block->sideband_set (sideband);

		result.resolved.push_back ({ current.hash, current_index, serialize_block_with_sideband (*current.block) });

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
		release_assert (backend.get_version (backend.tx_begin_read ()) == 24, "unexpected version during upgrade", std::to_string (backend.get_version (backend.tx_begin_read ())));

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

		constexpr size_t max_stack_depth = 10'000'000;

		auto const batch_size = nano::is_dev_run () ? 2 : 5000;

		auto const progress_step = std::max<uint64_t> (1, std::min<uint64_t> (total_blocks / 100, batch_size));

		for (uint64_t pass = 1;; ++pass)
		{
			logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: starting pass {}", pass);

			// Phase 1: Parallel DFS computation using for_each_par
			// Each thread iterates its range of blocks, performs bounded DFS with a read transaction,
			// and collects computed blocks locally. Results are merged after all threads complete.
			std::vector<computed_block> all_results;
			std::mutex results_mutex;
			std::atomic<uint64_t> iterated{ 0 };
			std::atomic<uint64_t> computed{ 0 };
			std::atomic<uint64_t> last_log_computed{ already_processed };

			block.for_each_par ([&] (nano::store::read_transaction const & read_tx, auto i, auto n) {
				std::vector<computed_block> local_results;

				for (; i != n; ++i)
				{
					iterated.fetch_add (1, std::memory_order_relaxed);

					auto const & hash = i->first;
					auto const & bws = i->second;
					auto const & blk = bws.block;

					if (topology.exists (read_tx, blk->sideband ().topo_index, hash))
					{
						continue; // Already processed
					}

					auto dfs = compute_topo_bounded (*this, read_tx, hash, max_stack_depth);
					if (!dfs.resolved.empty ())
					{
						auto const resolved_now = computed.fetch_add (dfs.resolved.size (), std::memory_order_relaxed) + dfs.resolved.size ();
						auto const total_computed = already_processed + resolved_now;
						auto const last_log = last_log_computed.load (std::memory_order_relaxed);
						double const pct = total_blocks == 0 ? 100.0 : (100.0 * static_cast<double> (total_computed) / static_cast<double> (total_blocks));
						if (total_computed - last_log >= progress_step)
						{
							last_log_computed.store (total_computed, std::memory_order_relaxed);
							// double const pct = total_blocks == 0 ? 100.0 : (100.0 * static_cast<double> (total_computed) / static_cast<double> (total_blocks));
							logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: computed {} / {} blocks ({:.2f}%), iterated {}", total_computed, total_blocks, pct, iterated.load (std::memory_order_relaxed));
						}
						logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: computed {} / {} blocks ({:.2f}%), iterated {}", total_computed, total_blocks, pct, iterated.load (std::memory_order_relaxed));
					}
					for (auto & item : dfs.resolved)
					{
						local_results.push_back (std::move (item));
					}
				}

				if (!local_results.empty ())
				{
					std::lock_guard lock{ results_mutex };
					all_results.insert (all_results.end (),
					std::make_move_iterator (local_results.begin ()),
					std::make_move_iterator (local_results.end ()));
				}
			});

			// Phase 2: Sequential write of all computed blocks
			auto write_tx = backend.tx_begin_write ();
			uint64_t written{ 0 };
			uint64_t last_log_written{ already_processed };

			for (auto & item : all_results)
			{
				// Skip duplicates (same block resolved by multiple threads)
				if (topology.exists (write_tx, item.topo_index, item.hash))
				{
					continue;
				}

				block.raw_put (write_tx, item.serialized, item.hash);
				topology.put (write_tx, item.topo_index, item.hash);
				++written;

				// Log progress at regular intervals
				auto const total_written = already_processed + written;
				if (total_written - last_log_written >= progress_step)
				{
					double const percentage = total_blocks == 0 ? 100.0 : (100.0 * static_cast<double> (total_written) / static_cast<double> (total_blocks));
					logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: wrote {} / {} blocks ({:.2f}%)", total_written, total_blocks, percentage);
					last_log_written = total_written;
				}

				if (written % batch_size == 0)
				{
					write_tx.refresh ();
				}
			}

			auto const total_done = topology.count (write_tx);
			double const percentage = total_blocks == 0 ? 100.0 : (100.0 * static_cast<double> (total_done) / static_cast<double> (total_blocks));
			logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: pass {} complete, resolved {} blocks this pass, total {} / {} ({:.2f}%)",
			pass, written, total_done, total_blocks, percentage);

			if (total_done >= total_blocks)
			{
				break;
			}

			release_assert (written > 0, "no progress made during topology rebuild pass", std::to_string (pass));

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
