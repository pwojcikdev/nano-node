#include <nano/lib/bounded_dfs.hpp>
#include <nano/store/ledger/account.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/confirmation_height.hpp>
#include <nano/store/ledger/final_vote.hpp>
#include <nano/store/ledger/online_weight.hpp>
#include <nano/store/ledger/peer.hpp>
#include <nano/store/ledger/pending.hpp>
#include <nano/store/ledger/pruned.hpp>
#include <nano/store/ledger/rep_weight.hpp>
#include <nano/store/ledger/topology.hpp>
#include <nano/store/ledger/version.hpp>
#include <nano/store/ledger_store.hpp>

namespace nano::store
{
// Builds a topological ordering index for every block in the ledger
// Each block is assigned an index that is strictly greater than all of its dependencies,
// The index is stored both in each block's sideband data and in a new `topology` table
void ledger_store::upgrade_v24_to_v25 ()
{
	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v24 to v25...");

	backend.open (schema_v25, nano::store::open_mode::read_write);
	{
		release_assert (backend.get_version (backend.tx_begin_read ()) == 24, "unexpected version during upgrade", std::to_string (backend.get_version (backend.tx_begin_read ())));

		// Clear topology table in case a previous upgrade attempt failed halfway
		topology.clear ();

		auto const total_blocks = block.count (backend.tx_begin_read ());
		logger.info (nano::log::type::ledger_upgrade, "Building topology index for {} blocks...", total_blocks);

		size_t const batch_size = nano::is_dev_run () ? 2 : 25000;
		size_t const max_depth = nano::is_dev_run () ? 16 : 16 * 1024 * 1024;
		size_t processed = 0;

		auto transaction = backend.tx_begin_write ();
		auto crawler = block.crawl (transaction);

		auto is_resolved = [&] (nano::block_hash const & hash) -> bool {
			if (hash.is_zero ())
			{
				return true;
			}
			auto blk = block.get (transaction, hash);
			if (!blk)
			{
				return true; // Pruned or external dependency
			}
			return blk->sideband ().topo_index != 0;
		};

		auto get_dependencies = [&] (nano::block_hash const & hash) -> std::array<nano::block_hash, 2> {
			auto blk = block.get (transaction, hash);
			release_assert (blk, "block must exist for dependency lookup", hash.to_string ());
			return blk->dependencies ();
		};

		auto resolve = [&] (nano::block_hash const & hash) -> bool {
			auto blk = block.get (transaction, hash);
			release_assert (blk, "block must exist for resolve", hash.to_string ());

			// Compute topo_index: 1 + max of all dependency topo indices (minimum 1)
			uint64_t topo = 1;
			auto deps = blk->dependencies ();
			for (auto const & dep : deps)
			{
				if (dep.is_zero ())
				{
					continue;
				}
				auto dep_block = block.get (transaction, dep);
				if (!dep_block)
				{
					continue; // Pruned or external
				}
				release_assert (dep_block->sideband ().topo_index != 0, "dependency must be resolved before dependent", dep.to_string ());
				topo = std::max (topo, dep_block->sideband ().topo_index + 1);
			}

			// Update block sideband with computed topo_index
			auto sideband = blk->sideband ();
			sideband.topo_index = topo;
			blk->sideband_set (sideband);
			block.put (transaction, hash, *blk);
			topology.put (transaction, topo, hash);

			++processed;
			if (processed % batch_size == 0)
			{
				auto percentage = total_blocks > 0 ? (processed * 100) / total_blocks : 0;
				logger.info (nano::log::type::ledger_upgrade, "Topology upgrade progress: {} / {} blocks ({}%)", processed, total_blocks, percentage);
				crawler.refresh ();
			}

			return true;
		};

		while (crawler)
		{
			auto const & [hash, bws] = *crawler;
			if (bws.sideband.topo_index != 0)
			{
				// Already resolved by a previous bounded_dfs call
				++crawler;
				continue;
			}

			nano::bounded_dfs (hash, max_depth, is_resolved, get_dependencies, resolve);

			logger.debug (nano::log::type::ledger_upgrade, "Processed block {} with hash {}", processed, hash.to_string ());

			++crawler;
		}

		logger.info (nano::log::type::ledger_upgrade, "Done processing {} blocks", processed);
		version.put (transaction, 25);
	}
	backend.close ();

	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v24 to v25 completed");
}
}