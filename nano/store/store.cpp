#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/ledger/account.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/confirmation_height.hpp>
#include <nano/store/ledger/final_vote.hpp>
#include <nano/store/ledger/online_weight.hpp>
#include <nano/store/ledger/peer.hpp>
#include <nano/store/ledger/pending.hpp>
#include <nano/store/ledger/pruned.hpp>
#include <nano/store/ledger/rep_weight.hpp>
#include <nano/store/ledger/version.hpp>
#include <nano/store/store.hpp>

namespace nano::store
{
ledger_store::ledger_store (std::unique_ptr<store::backend> backend_a, nano::stats & stats_a, nano::logger & logger_a) :
	backend_impl{ std::move (backend_a) },
	backend{ *backend_impl },
	stats{ stats_a },
	logger{ logger_a },
	block_impl{ std::make_unique<store::ledger::block> (backend) },
	account_impl{ std::make_unique<store::ledger::account> (backend) },
	pending_impl{ std::make_unique<store::ledger::pending> (backend) },
	rep_weight_impl{ std::make_unique<store::ledger::rep_weight> (backend) },
	online_weight_impl{ std::make_unique<store::ledger::online_weight> (backend) },
	pruned_impl{ std::make_unique<store::ledger::pruned> (backend) },
	peer_impl{ std::make_unique<store::ledger::peer> (backend) },
	confirmation_height_impl{ std::make_unique<store::ledger::confirmation_height> (backend) },
	final_vote_impl{ std::make_unique<store::ledger::final_vote> (backend) },
	version_impl{ std::make_unique<store::ledger::version> (backend) },
	block{ *block_impl },
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
	logger.info (nano::log::type::ledger_store, "Initializing ledger store: {}", backend.get_database_path ().string ());

	backend_meta meta{ 0 };

	bool needs_upgrade = false;
	bool fresh_db = false;
	{
		// Attempt to get meta information to determine if the exists or needs upgrading
		auto meta_result = backend.meta ();
		if (meta_result)
		{
			meta = meta_result.value ();

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
				logger.error (nano::log::type::ledger_store, "The version of the ledger database ({}) is lower than the minimum ({}) which is supported for upgrades. Perfrom an intermediate upgrade with an older node version or perform a fresh bootstrap.", meta.version, version_minimum);

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
			if (meta_result.error () == nano::error_backend::not_found)
			{
				fresh_db = true;

				logger.info (nano::log::type::ledger_store, "No existing ledger found, a new database will be created.");
			}
			else
			{
				throw std::runtime_error ("Failed to read meta information from the database: " + meta_result.error ().get_message ());
			}
		}
	}

	release_assert (meta.version > 0 || fresh_db);

	if (is_fresh_db)
	{
	}
}
}
