#include <nano/lib/block_type.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/db_val_templ.hpp>
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

#include <array>
#include <unordered_map>
#include <vector>

namespace nano::store
{
nano::store::column_schema const ledger_store::schema_current{
	{ nano::store::table::blocks, "blocks" },
	{ nano::store::table::accounts, "accounts" },
	{ nano::store::table::pending, "pending" },
	{ nano::store::table::rep_weights, "rep_weights" },
	{ nano::store::table::online_weight, "online_weight" },
	{ nano::store::table::pruned, "pruned" },
	{ nano::store::table::peers, "peers" },
	{ nano::store::table::confirmation_height, "confirmation_height" },
	{ nano::store::table::final_votes, "final_votes" },
	{ nano::store::table::topology, "topology" },
	{ nano::store::table::meta, "meta" }
};
}

namespace nano::store
{
ledger_store::ledger_store (std::unique_ptr<nano::store::backend> backend_a, nano::store::open_mode mode, nano::stats & stats_a, nano::logger & logger_a, ledger_store_params params) :
	stats{ stats_a },
	logger{ logger_a },
	backend_impl{ std::move (backend_a) },
	block_impl{ std::make_unique<nano::store::ledger::block_view> (*backend_impl) },
	account_impl{ std::make_unique<nano::store::ledger::account_view> (*backend_impl) },
	pending_impl{ std::make_unique<nano::store::ledger::pending_view> (*backend_impl) },
	rep_weight_impl{ std::make_unique<nano::store::ledger::rep_weight_view> (*backend_impl) },
	online_weight_impl{ std::make_unique<nano::store::ledger::online_weight_view> (*backend_impl) },
	pruned_impl{ std::make_unique<nano::store::ledger::pruned_view> (*backend_impl) },
	peer_impl{ std::make_unique<nano::store::ledger::peer_view> (*backend_impl) },
	confirmation_height_impl{ std::make_unique<nano::store::ledger::confirmation_height_view> (*backend_impl) },
	final_vote_impl{ std::make_unique<nano::store::ledger::final_vote_view> (*backend_impl) },
	topology_impl{ std::make_unique<nano::store::ledger::topology_view> (*backend_impl) },
	version_impl{ std::make_unique<nano::store::ledger::version_view> (*backend_impl) },
	backend{ *backend_impl },
	block{ *block_impl },
	account{ *account_impl },
	pending{ *pending_impl },
	rep_weight{ *rep_weight_impl },
	online_weight{ *online_weight_impl },
	pruned{ *pruned_impl },
	peer{ *peer_impl },
	confirmation_height{ *confirmation_height_impl },
	final_vote{ *final_vote_impl },
	topology{ *topology_impl },
	version{ *version_impl }
{
	// Skip automatic open/upgrade when defer_open is set (used for testing individual upgrades)
	if (params.defer_open)
	{
		return;
	}

	logger.info (nano::log::type::ledger_store, "Initializing ledger store: {}", backend.get_database_path ());

	bool needs_upgrade = false;
	bool fresh_db = false;
	backend_meta meta{};

	if (auto meta_opt = backend.fetch_meta ())
	{
		meta = *meta_opt;

		logger.debug (nano::log::type::ledger_store, "Ledger database version: {}", meta.version);

		// Prevent opening future database versions
		if (meta.version > version_current)
		{
			logger.error (nano::log::type::ledger_store, "The version of the ledger database ({}) is higher than the current ({}) which is supported. Either upgrade your node software or use a different database.", meta.version, version_current);

			throw std::runtime_error ("Ledger version " + std::to_string (meta.version) + " is higher than current version " + std::to_string (version_current));
		}

		// Minimum supported upgrade version check
		if (meta.version < version_minimum)
		{
			logger.error (nano::log::type::ledger_store, "The version of the ledger database ({}) is lower than the minimum ({}) which is supported for upgrades. Perform an intermediate upgrade with an older node version or perform a fresh bootstrap.", meta.version, version_minimum);

			throw std::runtime_error ("Ledger version " + std::to_string (meta.version) + " is lower than minimum supported version " + std::to_string (version_minimum));
		}

		// Check if upgrade is needed
		if (meta.version < version_current)
		{
			needs_upgrade = true;

			logger.info (nano::log::type::ledger_store, "The ledger database needs to be upgraded from version {} to {}", meta.version, version_current);
		}
	}
	else
	{
		fresh_db = true;

		logger.info (nano::log::type::ledger_store, "No existing ledger found, a new database will be created.");
	}
	release_assert (meta.version > 0 || fresh_db);

	if (needs_upgrade || fresh_db)
	{
		if (mode == nano::store::open_mode::read_only)
		{
			throw std::runtime_error ("Database requires upgrade but was opened in read-only mode");
		}
	}

	if (needs_upgrade)
	{
		if (params.backup_before_upgrade)
		{
			logger.info (nano::log::type::ledger_store, "Creating ledger backup before upgrade...");

			backend.open (backend::schema_meta, nano::store::open_mode::read_only);
			backend.backup ();
			backend.close ();

			logger.info (nano::log::type::ledger_store, "Ledger backup completed, continuing with upgrade...");
		}

		perform_upgrades (meta);
	}

	if (fresh_db)
	{
		logger.info (nano::log::type::ledger_store, "Creating new ledger database with version {} at '{}'",
		version_current, backend.get_database_path ());

		backend.create (schema_current, version_current);
	}

	backend.open (schema_current, mode);

	release_assert (backend.get_meta ().version == version_current, "ledger database version after initialization is not current");
}

ledger_store::~ledger_store () = default;

void ledger_store::initialize (nano::store::write_transaction const & txn, nano::ledger_constants const & constants)
{
	release_assert (empty (txn), "attempt to initialize a non-empty ledger store");
	release_assert (constants.genesis->has_sideband ());

	// TODO: Use designated initialization
	block.put (txn, constants.genesis->hash (), *constants.genesis);
	topology.put (txn, constants.genesis->sideband ().topo_index, constants.genesis->hash ());
	confirmation_height.put (txn, constants.genesis->account (), nano::confirmation_height_info{ 1, constants.genesis->hash () });
	account.put (txn, constants.genesis->account (), { constants.genesis->hash (), constants.genesis->account (), constants.genesis->hash (), std::numeric_limits<nano::uint128_t>::max (), nano::seconds_since_epoch (), 1, nano::epoch::epoch_0 });
	rep_weight.put (txn, constants.genesis->account (), std::numeric_limits<nano::uint128_t>::max ());
}

bool ledger_store::empty (nano::store::transaction const & txn) const
{
	for (auto const & [table, table_name] : schema_current)
	{
		if (table == nano::store::table::meta)
		{
			continue; // Ignore meta table
		}
		if (backend.begin (txn, table) != backend.end (txn, table))
		{
			return false;
		}
		debug_assert (backend.count (txn, table) == 0);
	}
	return true;
}

void ledger_store::perform_upgrades (nano::store::backend_meta meta)
{
	debug_assert (meta.version < version_current, "perform_upgrades called but no upgrade is necessary");
	release_assert (meta.version >= version_minimum, "perform_upgrades called but version is below minimum supported version", std::to_string (meta.version));

	switch (meta.version)
	{
		case 21:
			upgrade_v21_to_v22 ();
			[[fallthrough]];
		case 22:
			upgrade_v22_to_v23 ();
			[[fallthrough]];
		case 23:
			upgrade_v23_to_v24 ();
			[[fallthrough]];
		case 24:
			upgrade_v24_to_v25 ();
			[[fallthrough]];
		case 25:
			break;
		default:
			release_assert (false, "invalid ledger database version for upgrade", std::to_string (meta.version));
	}
}

/*
 * Upgrades
 */

nano::store::column_schema const ledger_store::schema_v21{
	{ nano::store::table::blocks, "blocks" },
	{ nano::store::table::accounts, "accounts" },
	{ nano::store::table::pending, "pending" },
	{ nano::store::table::online_weight, "online_weight" },
	{ nano::store::table::pruned, "pruned" },
	{ nano::store::table::peers, "peers" },
	{ nano::store::table::confirmation_height, "confirmation_height" },
	{ nano::store::table::final_votes, "final_votes" },
	{ nano::store::table::frontiers, "frontiers" },
	{ nano::store::table::unchecked, "unchecked" },
	{ nano::store::table::meta, "meta" }
};

nano::store::column_schema const ledger_store::schema_v22{
	{ nano::store::table::blocks, "blocks" },
	{ nano::store::table::accounts, "accounts" },
	{ nano::store::table::pending, "pending" },
	{ nano::store::table::online_weight, "online_weight" },
	{ nano::store::table::pruned, "pruned" },
	{ nano::store::table::peers, "peers" },
	{ nano::store::table::confirmation_height, "confirmation_height" },
	{ nano::store::table::final_votes, "final_votes" },
	{ nano::store::table::frontiers, "frontiers" },
	{ nano::store::table::meta, "meta" }
};

nano::store::column_schema const ledger_store::schema_v23{
	{ nano::store::table::blocks, "blocks" },
	{ nano::store::table::accounts, "accounts" },
	{ nano::store::table::pending, "pending" },
	{ nano::store::table::rep_weights, "rep_weights" },
	{ nano::store::table::online_weight, "online_weight" },
	{ nano::store::table::pruned, "pruned" },
	{ nano::store::table::peers, "peers" },
	{ nano::store::table::confirmation_height, "confirmation_height" },
	{ nano::store::table::final_votes, "final_votes" },
	{ nano::store::table::frontiers, "frontiers" },
	{ nano::store::table::meta, "meta" }
};

nano::store::column_schema const ledger_store::schema_v24{
	{ nano::store::table::blocks, "blocks" },
	{ nano::store::table::accounts, "accounts" },
	{ nano::store::table::pending, "pending" },
	{ nano::store::table::rep_weights, "rep_weights" },
	{ nano::store::table::online_weight, "online_weight" },
	{ nano::store::table::pruned, "pruned" },
	{ nano::store::table::peers, "peers" },
	{ nano::store::table::confirmation_height, "confirmation_height" },
	{ nano::store::table::final_votes, "final_votes" },
	{ nano::store::table::meta, "meta" }
};

nano::store::column_schema const ledger_store::schema_v25{
	{ nano::store::table::blocks, "blocks" },
	{ nano::store::table::accounts, "accounts" },
	{ nano::store::table::pending, "pending" },
	{ nano::store::table::rep_weights, "rep_weights" },
	{ nano::store::table::online_weight, "online_weight" },
	{ nano::store::table::pruned, "pruned" },
	{ nano::store::table::peers, "peers" },
	{ nano::store::table::confirmation_height, "confirmation_height" },
	{ nano::store::table::final_votes, "final_votes" },
	{ nano::store::table::topology, "topology" },
	{ nano::store::table::meta, "meta" }
};

// Drop unchecked table
void ledger_store::upgrade_v21_to_v22 ()
{
	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v21 to v22...");

	backend.open (schema_v21, nano::store::open_mode::read_write);
	{
		release_assert (backend.get_version (backend.tx_begin_read ()) == 21, "unexpected version during upgrade", std::to_string (backend.get_version (backend.tx_begin_read ())));

		backend.drop_table ("unchecked");

		auto transaction = backend.tx_begin_write ();
		backend.set_version (transaction, 22);
	}
	backend.close ();

	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v21 to v22 completed");
}

// Fill rep_weights table with all existing representatives and their vote weight
void ledger_store::upgrade_v22_to_v23 ()
{
	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v22 to v23...");

	// Open with schema_v23 so we can access rep_weights table
	// This allows us to drop it if a previous upgrade attempt failed halfway
	backend.open (schema_v23, nano::store::open_mode::read_write);
	{
		release_assert (backend.get_version (backend.tx_begin_read ()) == 22, "unexpected version during upgrade", std::to_string (backend.get_version (backend.tx_begin_read ())));

		// Always drop rep_weights table to ensure it's empty before populating
		// This can happen if an upgrade was attempted but failed halfway through
		backend.clear (nano::store::table::rep_weights);

		auto transaction = backend.tx_begin_write ();

		release_assert (rep_weight.begin (backend.tx_begin_read ()) == rep_weight.end (transaction), "rep weights table must be empty before upgrading to v23");

		auto iterate_accounts = [this] (auto && func) {
			auto transaction = backend.tx_begin_read ();

			// Manually create v22 compatible iterator to read accounts
			auto it = nano::store::typed_iterator<nano::account, nano::account_info_v22>{ backend.begin (transaction, nano::store::table::accounts) };
			auto const end = nano::store::typed_iterator<nano::account, nano::account_info_v22>{ backend.end (transaction, nano::store::table::accounts) };

			for (; it != end; ++it)
			{
				auto const & account = it->first;
				auto const & account_info = it->second;

				func (account, account_info);
			}
		};

		// Smaller batch size for dev runs to potentially trigger edge cases
		const size_t batch_size = nano::is_dev_run () ? 2 : 250000;

		size_t processed = 0;
		iterate_accounts ([this, &transaction, &processed, batch_size] (nano::account const & account, nano::account_info_v22 const & account_info) {
			if (!account_info.balance.is_zero ())
			{
				nano::uint128_t total{ 0 };
				nano::store::db_val value;
				auto status = backend.get (transaction, nano::store::table::rep_weights, account_info.representative, value);
				if (backend.success (status))
				{
					total = nano::amount{ value }.number ();
				}
				total += account_info.balance.number ();
				status = backend.put (transaction, nano::store::table::rep_weights, account_info.representative, nano::amount{ total });
				backend.release_assert_success (status);
			}

			processed++;
			if (processed % batch_size == 0)
			{
				logger.info (nano::log::type::ledger_upgrade, "Processed {} accounts", processed);
				transaction.refresh (); // Refresh to prevent excessive memory usage
			}
		});

		logger.info (nano::log::type::ledger_upgrade, "Done processing {} accounts", processed);
		version.put (transaction, 23);
	}
	backend.close ();

	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v22 to v23 completed");
}

// Drop frontiers table
void ledger_store::upgrade_v23_to_v24 ()
{
	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v23 to v24...");

	backend.open (schema_v23, nano::store::open_mode::read_write);
	{
		release_assert (backend.get_version (backend.tx_begin_read ()) == 23, "unexpected version during upgrade", std::to_string (backend.get_version (backend.tx_begin_read ())));

		backend.drop_table ("frontiers");

		auto transaction = backend.tx_begin_write ();
		version.put (transaction, 24);
	}
	backend.close ();

	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v23 to v24 completed");
}

namespace
{
	std::array<nano::block_hash, 2> topology_dependencies (nano::block const & block)
	{
		std::array<nano::block_hash, 2> result{ 0, 0 };

		switch (block.type ())
		{
			case nano::block_type::send:
			case nano::block_type::change:
				result[0] = block.previous ();
				break;
			case nano::block_type::receive:
				result[0] = block.previous ();
				result[1] = block.source_field ().value_or (0);
				break;
			case nano::block_type::open:
				result[0] = block.source_field ().value_or (0);
				break;
			case nano::block_type::state:
			{
				result[0] = block.previous ();
				auto const link = block.link_field ().value_or (0);
				if (!link.is_zero () && !block.sideband ().details.is_send && !block.sideband ().details.is_epoch)
				{
					result[1] = link.as_block_hash ();
				}
				break;
			}
			case nano::block_type::invalid:
			case nano::block_type::not_a_block:
				break;
		}

		return result;
	}

	std::vector<uint8_t> serialize_block_with_sideband (nano::block const & block)
	{
		std::vector<uint8_t> data;
		nano::vectorstream stream{ data };
		nano::serialize_block (stream, block);
		block.sideband ().serialize (stream, block.type ());
		return data;
	}
}

// Builds a topological ordering index for every block in the ledger.
// Each block is assigned an index that is strictly greater than all of its dependencies,
// allowing blocks to be iterated in dependency-respecting order without recomputing at runtime.
// The index is stored both in each block's sideband data and in a new `topology` table.
void ledger_store::upgrade_v24_to_v25 ()
{
	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v24 to v25...");

	backend.open (schema_v25, nano::store::open_mode::read_write);
	{
		release_assert (backend.get_version (backend.tx_begin_read ()) == 24, "unexpected version during upgrade", std::to_string (backend.get_version (backend.tx_begin_read ())));
		topology.clear ();
		logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: collecting block hashes...");

		// Collect all block hashes upfront using a short-lived read transaction
		std::vector<nano::block_hash> all_hashes;
		{
			auto read_tx = backend.tx_begin_read ();
			all_hashes.reserve (block.count (read_tx));

			for (auto i = backend.begin (read_tx, nano::store::table::blocks), n = backend.end (read_tx, nano::store::table::blocks); i != n; ++i)
			{
				nano::store::db_val key{ i->first };
				all_hashes.push_back (static_cast<nano::block_hash> (key));
			}
		}
		logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: {} blocks to process", all_hashes.size ());

		// Stack frame for the iterative DFS traversal
		struct frame
		{
			nano::block_hash hash{ 0 };
			std::array<nano::block_hash, 2> dependencies{ 0, 0 };
			size_t next_dependency{ 0 }; // Tracks which dependency to visit next
			std::shared_ptr<nano::block> block{};
		};

		auto transaction = backend.tx_begin_write ();

		// DFS state per block: 0 = unvisited, 1 = in-progress (on stack), 2 = fully processed
		std::unordered_map<nano::block_hash, uint8_t> states;
		states.reserve (all_hashes.size ());

		// Maps each processed block to its computed topology index
		std::unordered_map<nano::block_hash, uint64_t> topology_index;
		topology_index.reserve (all_hashes.size ());

		size_t processed = 0;
		const size_t batch_size = nano::is_dev_run () ? 2 : 250000;
		const size_t progress_step = std::max<size_t> (1, all_hashes.size () / 100); // 1% increments
		size_t next_progress = progress_step;
		logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: processing blocks...");

		// Iterative DFS over all blocks to compute topology indices
		std::vector<frame> stack;
		for (auto const & start_hash : all_hashes)
		{
			// Skip blocks that were already fully processed as dependencies of earlier blocks
			if (states[start_hash] == 2)
			{
				continue;
			}

			stack.clear ();
			stack.push_back ({ start_hash });

			while (!stack.empty ())
			{
				auto & current = stack.back ();
				auto & state = states[current.hash];

				// First visit: load the block and discover its dependencies
				if (state == 0)
				{
					current.block = block.get (transaction, current.hash);
					release_assert (current.block != nullptr, "missing block while rebuilding topology index", current.hash.to_string ());
					current.dependencies = topology_dependencies (*current.block);
					state = 1; // Mark as in-progress
				}

				// Try to recurse into unvisited dependencies before processing this block
				bool pushed = false;
				while (current.next_dependency < current.dependencies.size ())
				{
					auto const dependency = current.dependencies[current.next_dependency++];
					if (dependency.is_zero ())
					{
						continue;
					}
					release_assert (block.exists (transaction, dependency), "block dependency does not exist while rebuilding topology index", dependency.to_string ());

					auto dependency_state = states[dependency];
					if (dependency_state == 0)
					{
						// Unvisited dependency: push it onto the stack to process first
						stack.push_back ({ dependency });
						pushed = true;
						break;
					}
					if (dependency_state == 1)
					{
						// In-progress dependency means we've found a cycle in the block graph
						release_assert (false, "cycle detected while rebuilding topology index", dependency.to_string ());
					}
					// dependency_state == 2: already processed, will be used for index computation below
				}

				if (pushed)
				{
					continue; // Process the dependency before coming back to this block
				}

				// All dependencies are processed
				// Compute this block's topology index as max(dependency topology indices) + 1, ensuring it's strictly greater than all dependencies
				uint64_t current_index{ 0 };
				for (auto const & dependency : current.dependencies)
				{
					if (dependency.is_zero ())
					{
						continue;
					}

					auto existing = topology_index.find (dependency);
					release_assert (existing != topology_index.end (), "missing dependency topology index during migration", dependency.to_string ());
					current_index = std::max (current_index, existing->second + 1);
				}

				// Store the topology index in the block's sideband and in the topology table
				auto sideband = current.block->sideband ();
				sideband.topo_index = current_index;
				current.block->sideband_set (sideband);
				block.put (transaction, current.hash, *current.block);
				topology.put (transaction, sideband.topo_index, current.hash);
				topology_index.emplace (current.hash, sideband.topo_index);

				state = 2; // Mark as fully processed
				stack.pop_back ();

				processed++;

				bool should_log_progress = (processed >= next_progress) || (processed == all_hashes.size ());
				if (should_log_progress)
				{
					double const percentage = all_hashes.empty () ? 100.0 : (100.0 * static_cast<double> (processed) / static_cast<double> (all_hashes.size ()));
					logger.info (nano::log::type::ledger_upgrade, "Rebuilding topology index: processed {} / {} blocks ({:.2f}%)", processed, all_hashes.size (), percentage);
				}
				while (processed >= next_progress)
				{
					next_progress += progress_step;
				}

				// Periodically refresh the write transaction to avoid holding a long lock
				bool should_refresh = (processed % batch_size == 0);
				if (should_refresh)
				{
					transaction.refresh ();
				}
			}
		}

		// Verify every block was assigned exactly one topology entry
		release_assert (topology_index.size () == all_hashes.size (), "topology index rebuild count mismatch", std::to_string (topology_index.size ()) + " != " + std::to_string (all_hashes.size ()));
		release_assert (topology.count (transaction) == all_hashes.size (), "topology table count mismatch", std::to_string (topology.count (transaction)) + " != " + std::to_string (all_hashes.size ()));

		logger.info (nano::log::type::ledger_upgrade, "Done processing {} blocks", processed);
		version.put (transaction, 25);
	}
	backend.close ();

	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v24 to v25 completed");
}

std::string ledger_store::vendor_get () const
{
	return backend.vendor_get ();
}

std::filesystem::path ledger_store::get_database_path () const
{
	return backend.get_database_path ();
}

nano::store::open_mode ledger_store::get_mode () const
{
	release_assert (backend.get_mode ().has_value (), "ledger_store::get_mode called but backend is not open");
	return backend.get_mode ().value ();
}

uint64_t ledger_store::count (nano::store::transaction const & txn, nano::store::table table) const
{
	return backend.count (txn, table);
}

nano::store::write_transaction ledger_store::tx_begin_write ()
{
	return backend.tx_begin_write ();
}

nano::store::read_transaction ledger_store::tx_begin_read () const
{
	return backend.tx_begin_read ();
}
}
