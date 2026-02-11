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

struct dfs_frame
{
	nano::block_hash hash{ 0 };
	std::array<nano::block_hash, 2> dependencies{ 0, 0 };
	size_t next_dependency{ 0 };
	std::shared_ptr<nano::block> block{};
};

struct dfs_result
{
	uint64_t resolved_count{ 0 };
	bool overflowed{ false };
};

// Performs bounded iterative DFS to compute topo_index for a block and all its unresolved dependencies.
// Returns the number of blocks resolved. If the stack depth exceeds max_depth, returns with overflowed=true.
dfs_result compute_topo_bounded (
nano::store::ledger_store & store,
nano::store::write_transaction & write_tx,
nano::block_hash const & start_hash,
size_t max_depth)
{
	std::unordered_map<nano::block_hash, uint64_t> local_cache;
	local_cache.reserve (2048);

	std::vector<dfs_frame> stack;
	stack.reserve (2048);
	stack.push_back ({ start_hash });

	uint64_t resolved_count{ 0 };

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
			current.block = store.block.get (write_tx, current.hash);
			release_assert (current.block != nullptr, "missing block while rebuilding topology index", current.hash.to_string ());

			auto const persisted_index = current.block->sideband ().topo_index;
			if (store.topology.exists (write_tx, persisted_index, current.hash))
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
			auto dependency_block = store.block.get (write_tx, dependency);
			if (dependency_block == nullptr)
			{
				// Missing dependency (e.g. pruned source) treated as external
				continue;
			}

			auto const dependency_index = dependency_block->sideband ().topo_index;
			if (store.topology.exists (write_tx, dependency_index, dependency))
			{
				local_cache.emplace (dependency, dependency_index);
				continue;
			}

			// Unresolved dependency: check stack depth before pushing
			if (stack.size () >= max_depth)
			{
				return { resolved_count, true };
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

		// Write updated sideband and topology entry
		auto sideband = current.block->sideband ();
		sideband.topo_index = current_index;
		current.block->sideband_set (sideband);

		store.block.raw_put (write_tx, serialize_block_with_sideband (*current.block), current.hash);
		store.topology.put (write_tx, current_index, current.hash);

		local_cache.emplace (current.hash, current_index);
		++resolved_count;
		stack.pop_back ();
	}

	return { resolved_count, false };
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

		auto const is_lmdb = backend.vendor_get ().starts_with ("LMDB");
		auto const write_batch_size = is_lmdb ? static_cast<size_t> (5000) : static_cast<size_t> (250000);

		uint64_t const progress_step = std::max<uint64_t> (1, std::min<uint64_t> (total_blocks / 100, 250000));

		auto log_progress = [this, total_blocks] (uint64_t resolved, uint64_t iterated) {
			double const percentage = total_blocks == 0 ? 100.0 : (100.0 * static_cast<double> (resolved) / static_cast<double> (total_blocks));
			logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: resolved {} / {} blocks ({:.2f}%), iterated {}", resolved, total_blocks, percentage, iterated);
		};

		for (uint64_t pass = 1;; ++pass)
		{
			logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: starting pass {}", pass);

			uint64_t resolved_this_pass{ 0 };
			uint64_t iterated{ 0 };
			uint64_t last_log_resolved{ already_processed };

			// Iterate all blocks in batches using cursor-based reads
			nano::block_hash cursor{ 0 };
			bool has_cursor{ false };
			bool reached_end{ false };

			auto write_tx = backend.tx_begin_write ();
			uint64_t writes_since_refresh{ 0 };

			while (!reached_end)
			{
				// Read a batch of block hashes using a short-lived read transaction
				std::vector<nano::block_hash> batch;
				batch.reserve (4096);
				{
					auto read_tx = backend.tx_begin_read ();
					auto it = has_cursor ? block.begin (read_tx, cursor) : block.begin (read_tx);
					auto const end = block.end (read_tx);
					if (has_cursor && it != end && it->first == cursor)
					{
						++it; // Skip the cursor itself (already processed)
					}
					for (; it != end && batch.size () < 4096; ++it)
					{
						batch.push_back (it->first);
					}
					reached_end = (it == end);
				}

				if (batch.empty ())
				{
					break;
				}
				cursor = batch.back ();
				has_cursor = true;

				for (auto const & hash : batch)
				{
					++iterated;

					// Check if already resolved
					auto blk = block.get (write_tx, hash);
					release_assert (blk != nullptr, "missing block during topology rebuild", hash.to_string ());

					if (topology.exists (write_tx, blk->sideband ().topo_index, hash))
					{
						continue; // Already processed
					}

					auto result = compute_topo_bounded (*this, write_tx, hash, max_stack_depth);
					resolved_this_pass += result.resolved_count;
					writes_since_refresh += result.resolved_count;

					// Log progress at regular intervals
					auto const total_resolved = already_processed + resolved_this_pass;
					if (total_resolved - last_log_resolved >= progress_step)
					{
						log_progress (total_resolved, iterated);
						last_log_resolved = total_resolved;
					}

					if (writes_since_refresh >= write_batch_size)
					{
						write_tx.refresh ();
						writes_since_refresh = 0;
					}
				}
			}

			auto const total_done = topology.count (write_tx);
			double const percentage = total_blocks == 0 ? 100.0 : (100.0 * static_cast<double> (total_done) / static_cast<double> (total_blocks));
			logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: pass {} complete, resolved {} blocks this pass, total {} / {} ({:.2f}%)",
			pass, resolved_this_pass, total_done, total_blocks, percentage);

			if (total_done >= total_blocks)
			{
				break;
			}

			release_assert (resolved_this_pass > 0, "no progress made during topology rebuild pass", std::to_string (pass));

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
