#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/node/cementing_set.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/wallet.hpp>
#include <nano/node/wallet/wallets_receivable.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_cemented.hpp>
#include <nano/store/ledger/pending.hpp>

namespace nano::wallet
{
wallets_receivable::wallets_receivable (nano::wallet::wallets & wallets_a, nano::node & node_a, nano::ledger & ledger_a, nano::node_config const & config_a, nano::network_params const & network_params_a, nano::stats & stats_a, nano::logger & logger_a) :
	wallets{ wallets_a },
	node{ node_a },
	ledger{ ledger_a },
	config{ config_a },
	network_params{ network_params_a },
	stats{ stats_a },
	logger{ logger_a }
{
}

wallets_receivable::~wallets_receivable ()
{
	debug_assert (!thread.joinable ());
}

void wallets_receivable::start ()
{
	if (node.flags.disable_search_pending)
	{
		return;
	}
	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::wallet_receivable);
		run ();
	} };
}

void wallets_receivable::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

void wallets_receivable::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		lock.unlock ();

		stats.inc (nano::stat::type::wallet, nano::stat::detail::loop_receivable);

		// Reload wallets from disk
		wallets.reload ();

		// Search pending
		search_all ();

		lock.lock ();

		condition.wait_for (lock, network_params.node.search_pending_interval, [this] () {
			return stopped.load ();
		});
	}
}

bool wallets_receivable::search (nano::wallet_id const & id)
{
	// Snapshot the wallet accounts so the ledger scan below runs without touching the wallet store
	auto info = wallets.scan_info (id);
	if (!info)
	{
		if (info.error () == nano::error_common::wallet_locked)
		{
			logger.warn (nano::log::type::wallet, "Unable to search receivable blocks, wallet is locked. Blocks won't be auto-received until the wallet is unlocked");
		}
		return true;
	}

	logger.debug (nano::log::type::wallet, "Beginning receivable block search");

	auto const & scan = info.value ();
	for (auto const & account : scan.accounts)
	{
		auto ledger_txn = ledger.tx_begin_read ();
		for (auto j (ledger.store.pending.begin (ledger_txn, nano::pending_key (account, 0))), k (ledger.store.pending.end (ledger_txn)); j != k && nano::pending_key (j->first).account == account; ++j)
		{
			nano::pending_key key (j->first);
			auto hash (key.hash);
			nano::pending_info pending (j->second);
			auto amount (pending.amount.number ());
			if (config.receive_minimum.number () <= amount)
			{
				bool const confirmed = ledger.cemented.block_exists_or_pruned (ledger_txn, hash);

				logger.info (nano::log::type::wallet, "Found a receivable block: {} ({}) for account: {} from: {}",
				hash,
				confirmed ? "confirmed" : "unconfirmed",
				key.account,
				pending.source);

				if (confirmed)
				{
					// Receive confirmed block
					wallets.receive_async (id, hash, scan.representative, amount, account, [] (std::shared_ptr<nano::block> const &) {});
				}
				else if (!node.cementing_set.contains (hash))
				{
					auto block = ledger.any.block_get (ledger_txn, hash);
					if (block)
					{
						// Request confirmation for block which is not being processed yet
						node.start_election (block);
					}
				}
			}
		}
	}

	logger.debug (nano::log::type::wallet, "Receivable block search phase complete");
	return false;
}

void wallets_receivable::search_all ()
{
	for (auto const & id : wallets.wallet_ids ())
	{
		search (id);
	}
}

void wallets_receivable::receive_confirmed (nano::block_hash const & hash, nano::account const & destination)
{
	for (auto const & [id, representative] : wallets.holders (destination))
	{
		auto pending = ledger.any.pending_get (ledger.tx_begin_read (), nano::pending_key (destination, hash));
		if (pending)
		{
			auto amount (pending->amount.number ());
			wallets.receive_async (id, hash, representative, amount, destination, [] (std::shared_ptr<nano::block> const &) {});
		}
		else
		{
			if (!ledger.cemented.block_exists_or_pruned (ledger.tx_begin_read (), hash))
			{
				logger.warn (nano::log::type::wallet, "Confirmed block is missing: {}", hash);
				debug_assert (false, "confirmed block is missing");
			}
			else
			{
				logger.warn (nano::log::type::wallet, "Block has already been received: {}", hash);
			}
		}
	}
}
}
