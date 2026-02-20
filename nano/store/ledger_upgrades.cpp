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

		size_t const batch_size = nano::is_dev_run () ? 2 : 1024 * 1024;
		size_t const max_depth = nano::is_dev_run () ? 16 : 16 * 1024 * 1024;

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
					// nullptr means pruned or external, will be filtered by is_resolved
				}
			}
			return deps;
		};

		auto verify_topo = [&] (std::shared_ptr<nano::block> const & blk) {
			auto topo = blk->sideband ().topo_index;
			for (auto const & dep_hash : blk->dependencies ())
			{
				if (dep_hash.is_zero ())
				{
					continue;
				}
				auto dep_block = block.get (transaction, dep_hash);
				release_assert (!dep_block || dep_block->sideband ().topo_index < topo,
				"topo ordering violation",
				"block " + blk->hash ().to_string () + " topo=" + std::to_string (topo) + " dep " + dep_hash.to_string () + " dep_topo=" + std::to_string (dep_block ? dep_block->sideband ().topo_index : 0));
			}
		};

		size_t resolved = 0;

		auto resolve = [&] (std::shared_ptr<nano::block> const & blk) -> bool {
			// Compute topo_index: 1 + max of all dependency topo indices (minimum 1)
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

			// Update block sideband with computed topo_index
			auto const hash = blk->hash ();
			auto sideband = blk->sideband ();
			sideband.topo_index = topo;
			blk->sideband_set (sideband);
			block.put (transaction, hash, *blk);
			// topology.put (transaction, topo, hash); // TODO: Phase 2: Add separate topology table for reverse lookup

			verify_topo (blk);

			++resolved;

			// Periodic progress logging
			auto now = std::chrono::steady_clock::now ();
			if (now - last_log >= std::chrono::seconds (5))
			{
				auto percentage = total_blocks > 0 ? (resolved * 100) / total_blocks : 0;
				logger.info (nano::log::type::ledger_upgrade, "Topology resolve progress: {} / {} blocks ({}%)", resolved, total_blocks, percentage);
				last_log = now;
			}

			// Commit periodically
			if (resolved % batch_size == 0)
			{
				logger.debug (nano::log::type::ledger_upgrade, "Committing batch of {} blocks...", batch_size);
				auto const refresh_start = std::chrono::steady_clock::now ();
				crawler.refresh ();
				auto const refresh_ms = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - refresh_start).count ();
				logger.debug (nano::log::type::ledger_upgrade, "Transaction refresh took {}ms", refresh_ms);
			}

			return true;
		};

		// Iterate over all blocks
		size_t processed = 0;
		for (; crawler; ++crawler, ++processed)
		{
			auto const & [hash, bws] = *crawler;

			if (processed % batch_size == 0)
			{
				auto percentage = total_blocks > 0 ? (processed * 100) / total_blocks : 0;
				logger.info (nano::log::type::ledger_upgrade, "Processing progress: {} / {} blocks ({}%)", processed, total_blocks, percentage);
			}

			if (bws.sideband.topo_index != 0)
			{
				// Already resolved by a previous bounded_dfs call
				verify_topo (bws.block);
				continue;
			}

			nano::bounded_dfs_result dfs_result;
			do
			{
				dfs_result = nano::bounded_dfs (bws.block, max_depth, is_resolved, get_dependencies, resolve);
				if (dfs_result.overflow)
				{
					logger.debug (nano::log::type::ledger_upgrade, "Partially resolved {} dependencies for block {}, continuing...", dfs_result.resolved, hash);
				}
			} while (dfs_result.overflow);

			logger.debug (nano::log::type::ledger_upgrade, "Processed block {} with hash {}", processed, hash);
		}
		crawler.reset (); // Release crawler iterators before refreshing transaction

		// Commit
		transaction.refresh ();

		logger.info (nano::log::type::ledger_upgrade, "Verifying topology index...");

		// Verify that all blocks have been assigned a topo_index and that the ordering is correct
		{
			size_t verified = 0;
			for (auto it = block.begin (transaction), end = block.end (transaction); it != end; ++it)
			{
				auto const & [hash, bws] = *it;
				auto const & blk = bws.block;
				release_assert (blk->sideband ().topo_index != 0, "block missing topo_index after v24->v25 upgrade", hash.to_string ());
				verify_topo (blk);

				++verified;

				if (verified % batch_size == 0)
				{
					auto percentage = total_blocks > 0 ? (verified * 100) / total_blocks : 0;
					logger.info (nano::log::type::ledger_upgrade, "Topology verification progress: {} / {} blocks ({}%)", verified, total_blocks, percentage);
				}
			}
		}

		logger.info (nano::log::type::ledger_upgrade, "Done processing {} blocks", processed);
		version.put (transaction, 25);
	}
	backend.close ();

	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v24 to v25 completed");
}

// Populates the topology table from sideband topo_index values computed in v24->v25
void ledger_store::upgrade_v25_to_v26 ()
{
	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v25 to v26...");

	backend.open (schema_v26, nano::store::open_mode::read_write);
	{
		release_assert (backend.get_version (backend.tx_begin_read ()) == 25, "unexpected version during upgrade", std::to_string (backend.get_version (backend.tx_begin_read ())));

		// Clear topology table in case a previous upgrade attempt failed halfway
		topology.clear ();

		auto const total_blocks = block.count (backend.tx_begin_read ());
		logger.info (nano::log::type::ledger_upgrade, "Populating topology table for {} blocks...", total_blocks);

		size_t const batch_size = nano::is_dev_run () ? 2 : 16 * 1024 * 1024;
		size_t processed = 0;
		auto last_log = std::chrono::steady_clock::now ();

		auto transaction = backend.tx_begin_write ();
		auto crawler = block.crawl (transaction);

		while (crawler)
		{
			auto const & [hash, bws] = *crawler;
			auto const topo = bws.sideband.topo_index;
			release_assert (topo != 0, "block missing topo_index during v25->v26 upgrade", hash.to_string ());

			topology.put (transaction, topo, hash);

			++processed;

			// Periodic progress logging
			auto now = std::chrono::steady_clock::now ();
			if (now - last_log >= std::chrono::seconds (5))
			{
				auto percentage = total_blocks > 0 ? (processed * 100) / total_blocks : 0;
				logger.info (nano::log::type::ledger_upgrade, "Topology table progress: {} / {} blocks ({}%)", processed, total_blocks, percentage);
				last_log = now;
			}

			if (processed % batch_size == 0)
			{
				logger.debug (nano::log::type::ledger_upgrade, "Committing batch of {} blocks...", batch_size);
				auto const refresh_start = std::chrono::steady_clock::now ();
				crawler.refresh ();
				auto const refresh_ms = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - refresh_start).count ();
				logger.debug (nano::log::type::ledger_upgrade, "Transaction refresh took {}ms", refresh_ms);
			}

			++crawler;
		}

		logger.info (nano::log::type::ledger_upgrade, "Done populating topology table for {} blocks", processed);
		version.put (transaction, 26);
	}
	backend.close ();

	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v25 to v26 completed");
}
}