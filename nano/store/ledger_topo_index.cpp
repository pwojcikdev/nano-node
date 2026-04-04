#include <nano/lib/bounded_dfs.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/topology.hpp>
#include <nano/store/ledger/version.hpp>
#include <nano/store/ledger_store.hpp>

#include <array>
#include <chrono>

namespace nano::store
{
// Computes and stores topological ordering index for every block in the ledger.
// Each block is assigned an index strictly greater than all of its dependencies.
// The index is stored both in each block's sideband data and in the topology table.
// This is an expensive operation intended to be run as an explicit CLI upgrade step.
void ledger_store::populate_topo_index ()
{
	logger.info (nano::log::type::ledger_upgrade, "Populating topology index...");

	auto const total_blocks = block.count (backend.tx_begin_read ());
	logger.info (nano::log::type::ledger_upgrade, "Building topology index for {} blocks...", total_blocks);

	size_t const batch_size = nano::is_dev_run () ? 2 : 1024 * 1024;
	size_t const max_depth = nano::is_dev_run () ? 16 : 16 * 1024 * 1024;

	// Phase 1: Compute topo_index for all blocks using bounded DFS
	{
		topology.clear ();

		auto last_log = std::chrono::steady_clock::now ();

		auto transaction = backend.tx_begin_write ();
		auto crawler = block.crawl (transaction);

		auto is_resolved = [&] (std::shared_ptr<nano::block> const & blk) -> bool {
			if (!blk)
			{
				return true; // Pruned or external dependency
			}
			return blk->sideband ().topo_index != 0;
		};

		auto get_dependencies = [&] (std::shared_ptr<nano::block> const & blk) -> std::array<std::shared_ptr<nano::block>, 2> {
			auto dep_hashes = blk->dependencies ();
			std::array<std::shared_ptr<nano::block>, 2> deps{};
			for (size_t i = 0; i < dep_hashes.size (); ++i)
			{
				auto const & dep_hash = dep_hashes[i];
				if (!dep_hash.is_zero ())
				{
					deps[i] = block.get (transaction, dep_hash);
				}
			}
			return deps;
		};

		size_t resolved = 0;

		auto resolve = [&] (std::shared_ptr<nano::block> const & blk) -> bool {
			uint64_t topo = 1;
			auto dep_hashes = blk->dependencies ();
			for (auto const & dep_hash : dep_hashes)
			{
				if (dep_hash.is_zero ())
				{
					continue;
				}
				auto dep_block = block.get (transaction, dep_hash);
				if (!dep_block)
				{
					continue; // Pruned or external
				}
				release_assert (dep_block->sideband ().topo_index != 0, "dependency must be resolved before dependent", dep_hash.to_string ());
				topo = std::max (topo, dep_block->sideband ().topo_index + 1);
			}

			auto const hash = blk->hash ();
			auto sideband = blk->sideband ();
			sideband.topo_index = topo;
			blk->sideband_set (sideband);
			block.put (transaction, hash, *blk);
			topology.put (transaction, topo, hash);

			++resolved;

			auto now = std::chrono::steady_clock::now ();
			if (now - last_log >= std::chrono::seconds (5))
			{
				auto percentage = total_blocks > 0 ? (resolved * 100) / total_blocks : 0;
				logger.info (nano::log::type::ledger_upgrade, "Topology resolve progress: {} / {} blocks ({}%)", resolved, total_blocks, percentage);
				last_log = now;
			}

			if (resolved % batch_size == 0)
			{
				crawler.refresh ();
			}

			return true;
		};

		size_t processed = 0;
		for (; crawler; ++crawler, ++processed)
		{
			auto const & [hash, bws] = *crawler;

			if (bws.sideband.topo_index != 0)
			{
				continue; // Already resolved by a previous bounded_dfs call
			}

			nano::bounded_dfs_result dfs_result;
			do
			{
				dfs_result = nano::bounded_dfs (bws.block, max_depth, is_resolved, get_dependencies, resolve);
			} while (dfs_result.overflow);
		}

		logger.info (nano::log::type::ledger_upgrade, "Resolved {} blocks", resolved);
	}

	// Phase 2: Verify and set enabled flag
	{
		auto transaction = backend.tx_begin_write ();

		// Verify ordering
		size_t verified = 0;
		for (auto it = block.begin (transaction), end = block.end (transaction); it != end; ++it)
		{
			auto const & [hash, bws] = *it;
			auto const & blk = bws.block;
			auto topo = blk->sideband ().topo_index;
			release_assert (topo != 0, "block missing topo_index after populate", hash.to_string ());

			for (auto const & dep_hash : blk->dependencies ())
			{
				if (dep_hash.is_zero ())
				{
					continue;
				}
				auto dep_block = block.get (transaction, dep_hash);
				release_assert (!dep_block || dep_block->sideband ().topo_index < topo,
				"topo ordering violation",
				"block " + hash.to_string () + " topo=" + std::to_string (topo) + " dep " + dep_hash.to_string ());
			}
			++verified;
		}

		version.put_topo_enabled (transaction, true);
		logger.info (nano::log::type::ledger_upgrade, "Topology index populated and verified for {} blocks", verified);
	}
}
}
