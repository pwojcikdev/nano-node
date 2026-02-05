#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/stream.hpp>
#include <nano/lib/block_type.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/db_val.hpp>
#include <nano/store/ledger/account.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/blocks_topo.hpp>
#include <nano/store/ledger/confirmation_height.hpp>
#include <nano/store/ledger/final_vote.hpp>
#include <nano/store/ledger/online_weight.hpp>
#include <nano/store/ledger/peer.hpp>
#include <nano/store/ledger/pending.hpp>
#include <nano/store/ledger/pruned.hpp>
#include <nano/store/ledger/rep_weight.hpp>
#include <nano/store/ledger/version.hpp>
#include <nano/store/ledger_store.hpp>

#include <boost/endian/conversion.hpp>

#include <algorithm>
#include <optional>
#include <vector>

namespace nano::store
{
nano::store::column_schema const ledger_store::schema_current{
	{ nano::store::table::blocks, "blocks" },
	{ nano::store::table::blocks_topo, "blocks_topo" },
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
}

namespace nano::store
{
ledger_store::ledger_store (std::unique_ptr<nano::store::backend> backend_a, nano::store::open_mode mode, nano::stats & stats_a, nano::logger & logger_a, ledger_store_params params) :
	stats{ stats_a },
	logger{ logger_a },
	backend_impl{ std::move (backend_a) },
	block_impl{ std::make_unique<nano::store::ledger::block_view> (*backend_impl) },
	blocks_topo_impl{ std::make_unique<nano::store::ledger::blocks_topo_view> (*backend_impl) },
	account_impl{ std::make_unique<nano::store::ledger::account_view> (*backend_impl) },
	pending_impl{ std::make_unique<nano::store::ledger::pending_view> (*backend_impl) },
	rep_weight_impl{ std::make_unique<nano::store::ledger::rep_weight_view> (*backend_impl) },
	online_weight_impl{ std::make_unique<nano::store::ledger::online_weight_view> (*backend_impl) },
	pruned_impl{ std::make_unique<nano::store::ledger::pruned_view> (*backend_impl) },
	peer_impl{ std::make_unique<nano::store::ledger::peer_view> (*backend_impl) },
	confirmation_height_impl{ std::make_unique<nano::store::ledger::confirmation_height_view> (*backend_impl) },
	final_vote_impl{ std::make_unique<nano::store::ledger::final_vote_view> (*backend_impl) },
	version_impl{ std::make_unique<nano::store::ledger::version_view> (*backend_impl) },
	backend{ *backend_impl },
	block{ *block_impl },
	blocks_topo{ *blocks_topo_impl },
	account{ *account_impl },
	pending{ *pending_impl },
	rep_weight{ *rep_weight_impl },
	online_weight{ *online_weight_impl },
	pruned{ *pruned_impl },
	peer{ *peer_impl },
	confirmation_height{ *confirmation_height_impl },
	final_vote{ *final_vote_impl },
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
	blocks_topo.put (txn, constants.genesis->sideband ().topo_height, constants.genesis->hash ());
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
	{ nano::store::table::blocks_topo, "blocks_topo" },
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

// Add per-block topo_height (stored in sideband) and build blocks_topo index table
void ledger_store::upgrade_v24_to_v25 ()
{
	logger.info (nano::log::type::ledger_upgrade, "Upgrading database from v24 to v25...");

	backend.open (schema_v25, nano::store::open_mode::read_write);
	{
		release_assert (backend.get_version (backend.tx_begin_read ()) == 24, "unexpected version during upgrade", std::to_string (backend.get_version (backend.tx_begin_read ())));

		// Always clear index table to ensure it's empty before rebuilding
		auto status = backend.clear (nano::store::table::blocks_topo);
		backend.release_assert_success (status);

		auto write_txn = backend.tx_begin_write ();

		auto read_entry = [this] (nano::store::transaction const & txn, nano::block_hash const & hash, std::shared_ptr<nano::block> & block, nano::block_sideband & sideband, nano::block_type & type) {
			nano::store::db_val data;
			auto status = backend.get (txn, nano::store::table::blocks, hash, data);
			release_assert (backend.success (status) || backend.not_found (status), backend.error_string (status));
			if (backend.not_found (status) || data.size () == 0)
			{
				return false;
			}

			nano::bufferstream stream{ reinterpret_cast<uint8_t const *> (data.data ()), data.size () };
			bool error = nano::try_read (stream, type);
			release_assert (!error);
			block = nano::deserialize_block (stream, type);
			release_assert (block != nullptr);
			error = sideband.deserialize (stream, type);
			release_assert (!error);
			return true;
		};

		auto get_topo = [&] (nano::block_hash const & hash) -> uint64_t {
			if (hash.is_zero ())
			{
				return 0;
			}
			if (pruned.exists (write_txn, hash))
			{
				return 0;
			}
			std::shared_ptr<nano::block> block;
			nano::block_sideband sideband;
			nano::block_type type{};
			if (!read_entry (write_txn, hash, block, sideband, type))
			{
				return 0;
			}
			return sideband.topo_height;
		};

		auto compute_and_store = [&] (nano::block_hash const & target) -> uint64_t {
			std::vector<nano::block_hash> stack;
			stack.push_back (target);

			while (!stack.empty ())
			{
				auto const hash = stack.back ();
				if (hash.is_zero () || pruned.exists (write_txn, hash))
				{
					stack.pop_back ();
					continue;
				}

				std::shared_ptr<nano::block> block;
				nano::block_sideband sideband;
				nano::block_type type{};
				if (!read_entry (write_txn, hash, block, sideband, type))
				{
					stack.pop_back ();
					continue;
				}

				// If topo is present, just ensure the index entry exists
				if (sideband.topo_height != 0)
				{
					blocks_topo.put (write_txn, sideband.topo_height, hash);
					stack.pop_back ();
					continue;
				}

				auto previous = block->previous ();
				nano::block_hash source{ 0 };

				switch (type)
				{
					case nano::block_type::receive:
					case nano::block_type::open:
					{
						source = block->source_field ().value_or (0);
					}
					break;
					case nano::block_type::state:
					{
						if (sideband.details.is_receive)
						{
							auto const & state = static_cast<nano::state_block const &> (*block);
							source = state.hashables.link.as_block_hash ();
						}
					}
					break;
					case nano::block_type::send:
					case nano::block_type::change:
						break;
					default:
						release_assert (false, "invalid block type during upgrade");
				}

				bool pushed_dependency = false;
				auto try_push_dependency = [&] (nano::block_hash const & dep) {
					if (pushed_dependency || dep.is_zero () || pruned.exists (write_txn, dep))
					{
						return;
					}
					if (get_topo (dep) != 0)
					{
						return;
					}
					if (std::find (stack.begin (), stack.end (), dep) != stack.end ())
					{
						release_assert (false, "cycle detected while computing topo_height");
					}
					stack.push_back (dep);
					pushed_dependency = true;
				};

				try_push_dependency (previous);
				try_push_dependency (source);

				if (pushed_dependency)
				{
					continue;
				}

				auto const prev_topo = get_topo (previous);
				auto const source_topo = get_topo (source);
				auto const topo = std::max (prev_topo, source_topo) + 1;

				sideband.topo_height = topo;

				std::vector<uint8_t> out;
				{
					nano::vectorstream stream{ out };
					nano::serialize_block (stream, *block);
					sideband.serialize (stream, type);
				}
				auto status = backend.put (write_txn, nano::store::table::blocks, hash, nano::store::db_val{ out.size (), out.data () });
				backend.release_assert_success (status);

				blocks_topo.put (write_txn, topo, hash);

				stack.pop_back ();
			}

			return get_topo (target);
		};

		// Smaller batch size for dev runs to potentially trigger edge cases
		uint64_t const batch_size = nano::is_dev_run () ? 1000 : 50000;

		uint64_t processed = 0;
		std::optional<nano::block_hash> start_hash;
		while (true)
		{
			auto read_txn = backend.tx_begin_read ();

			auto it = start_hash ? backend.begin (read_txn, nano::store::table::blocks, *start_hash) : backend.begin (read_txn, nano::store::table::blocks);
			auto const end = backend.end (read_txn, nano::store::table::blocks);

			if (it == end)
			{
				break;
			}

			for (; it != end; ++it)
			{
				auto const & [key, value] = *it;
				(void)value;

				nano::store::db_val db_key{ key };
				nano::block_hash hash = db_key.convert_to<nano::block_hash> ();

				compute_and_store (hash);

				processed++;
				if (processed % batch_size == 0)
				{
					logger.info (nano::log::type::ledger_upgrade, "Processed {} blocks", processed);

					write_txn.refresh (); // Refresh to prevent excessive memory usage

					start_hash = nano::block_hash{ hash.number () + 1 };
					break; // Restart iterators with refreshed transactions
				}
			}

			// Reached end without hitting batch refresh
			if (it == end)
			{
				break;
			}
		}

		logger.info (nano::log::type::ledger_upgrade, "Done processing {} blocks", processed);
		version.put (write_txn, 25);
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
