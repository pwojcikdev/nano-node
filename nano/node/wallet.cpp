#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/formatting.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/threading.hpp>
#include <nano/lib/utility.hpp>
#include <nano/lib/work_version.hpp>
#include <nano/node/cementing_set.hpp>
#include <nano/node/election.hpp>
#include <nano/node/node.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_cemented.hpp>
#include <nano/store/ledger/pending.hpp>
#include <nano/store/lmdb/common.hpp>
#include <nano/store/lmdb/db_val.hpp>
#include <nano/store/lmdb/iterator.hpp>
#include <nano/store/lmdb/utility.hpp>

#include <boost/format.hpp>
#include <boost/polymorphic_cast.hpp>

#include <future>
#include <stdexcept>

#include <argon2.h>

nano::mdb_wallets_store::mdb_wallets_store (std::filesystem::path const & path_a, nano::lmdb_config const & lmdb_config_a) :
	environment (path_a, nano::store::lmdb::env::options::make ().set_config (lmdb_config_a).override_config_sync (nano::lmdb_config::sync_strategy::always).override_config_map_size (1ULL * 1024 * 1024 * 1024))
{
}

/*
 * wallet
 */

std::shared_ptr<nano::wallet> nano::wallet::open_existing (nano::store::write_transaction & txn_wallet, nano::wallets & wallets, std::string const & wallet_path)
{
	auto result = std::shared_ptr<nano::wallet>{ new nano::wallet{ wallets } };
	result->store.open (txn_wallet, wallet_path);
	return result;
}

std::shared_ptr<nano::wallet> nano::wallet::create (nano::store::write_transaction & txn_wallet, nano::wallets & wallets, std::string const & wallet_path, nano::account representative)
{
	auto result = std::shared_ptr<nano::wallet>{ new nano::wallet{ wallets } };
	result->store.create (txn_wallet, representative, wallet_path);
	return result;
}

std::shared_ptr<nano::wallet> nano::wallet::create_from_json (nano::store::write_transaction & txn_wallet, nano::wallets & wallets, std::string const & wallet_path, std::string const & json)
{
	auto result = std::shared_ptr<nano::wallet>{ new nano::wallet{ wallets } };
	result->store.create_from_json (txn_wallet, wallet_path, json);
	return result;
}

nano::wallet::wallet (nano::wallets & wallets) :
	lock_observer ([] (bool, bool) {}),
	store (wallets.kdf, wallets.env, wallets.config.password_fanout),
	wallets (wallets),
	logger (wallets.logger)
{
}

void nano::wallet::enter_initial_password ()
{
	nano::raw_key password_l;
	{
		nano::lock_guard<std::recursive_mutex> lock{ store.mutex };
		store.password.value (password_l);
	}
	if (password_l.is_zero ())
	{
		auto transaction (wallets.tx_begin_read ());
		enter_password_impl (transaction, wallet_store::default_password);
	}
}

bool nano::wallet::enter_password (std::string const & password_a)
{
	bool result;
	{
		auto transaction = wallets.tx_begin_write ();
		result = enter_password_impl (transaction, password_a);
	}
	if (!result)
	{
		wallets.refresh_rep_keys_cache ();
	}
	return result;
}

bool nano::wallet::enter_password_impl (nano::store::transaction const & transaction_a, std::string const & password_a)
{
	auto result (store.attempt_password (transaction_a, password_a));
	if (!result)
	{
		logger.info (nano::log::type::wallet, "Wallet unlocked");

		auto this_l = shared_from_this ();
		wallets.queue_wallet_action (nano::wallets::high_priority, this_l, [this_l] (nano::wallet & wallet) {
			// Wallets must survive node lifetime
			this_l->search_receivable ();
		});
	}
	else
	{
		logger.warn (nano::log::type::wallet, "Invalid password, wallet locked");
	}
	lock_observer (result, password_a.empty ());
	return result;
}

nano::public_key nano::wallet::deterministic_insert_impl (nano::store::write_transaction const & transaction, bool generate_work)
{
	auto key = store.deterministic_insert (transaction);

	logger.info (nano::log::type::wallet, "Deterministically inserted new account: {}", key.to_account ());

	if (generate_work)
	{
		work_ensure (key, key);
	}

	if (wallets.check_rep (key))
	{
		logger.info (nano::log::type::wallet, "New account qualified as a representative: {}", key.to_account ());
		representatives.lock ()->insert (key);
	}

	return key;
}

nano::public_key nano::wallet::deterministic_insert_impl (nano::store::write_transaction const & transaction, uint32_t index, bool generate_work)
{
	auto key = store.deterministic_insert (transaction, index);

	logger.info (nano::log::type::wallet, "Deterministically inserted new account: {} with index: {}", key.to_account (), index);

	if (generate_work)
	{
		work_ensure (key, key);
	}

	if (wallets.check_rep (key))
	{
		logger.info (nano::log::type::wallet, "New account qualified as a representative: {}", key.to_account ());
		representatives.lock ()->insert (key);
	}

	return key;
}

nano::result<nano::public_key> nano::wallet::deterministic_insert (uint32_t index, bool generate_work)
{
	auto transaction = wallets.tx_begin_write ();

	if (!store.valid_password (transaction))
	{
		return nano::error (nano::error_common::wallet_locked);
	}

	return deterministic_insert_impl (transaction, index, generate_work);
}

nano::result<nano::public_key> nano::wallet::deterministic_insert (bool generate_work)
{
	auto transaction = wallets.tx_begin_write ();

	if (!store.valid_password (transaction))
	{
		return nano::error (nano::error_common::wallet_locked);
	}

	auto result = deterministic_insert_impl (transaction, generate_work);
	transaction.commit ();
	wallets.refresh_rep_keys_cache ();
	return result;
}

nano::result<nano::public_key> nano::wallet::insert_adhoc (nano::raw_key const & prv, bool generate_work)
{
	auto transaction = wallets.tx_begin_write ();

	if (!store.valid_password (transaction))
	{
		return nano::error (nano::error_common::wallet_locked);
	}

	auto key = store.insert_adhoc (transaction, prv);

	logger.info (nano::log::type::wallet, "Ad-hoc inserted new account: {}", key.to_account ());

	if (generate_work)
	{
		auto ledger_txn = wallets.ledger.tx_begin_read ();
		work_ensure (key, wallets.ledger.latest_root (ledger_txn, key));
	}

	// Makes sure that the representatives container will be in sync with any added keys
	transaction.commit ();

	if (wallets.check_rep (key))
	{
		logger.info (nano::log::type::wallet, "New account qualified as a representative: {}", key.to_account ());
		representatives.lock ()->insert (key);
		wallets.refresh_rep_keys_cache ();
	}

	return key;
}

bool nano::wallet::insert_watch (nano::public_key const & pub_a)
{
	auto transaction = wallets.tx_begin_write ();
	return insert_watch_impl (transaction, pub_a);
}

bool nano::wallet::insert_watch_impl (nano::store::write_transaction const & transaction_a, nano::public_key const & pub_a)
{
	return store.insert_watch (transaction_a, pub_a);
}

bool nano::wallet::exists (nano::public_key const & account_a)
{
	auto transaction (wallets.tx_begin_read ());
	return store.exists (transaction, account_a);
}

bool nano::wallet::import (std::string const & json, std::string const & password)
{
	bool error (true);
	auto txn_wallet = wallets.tx_begin_write ();
	nano::uint256_union id;
	random_pool::generate_block (id.bytes.data (), id.bytes.size ());
	try
	{
		nano::wallet_store temp{ wallets.kdf, wallets.env, 1 };
		temp.create_from_json (txn_wallet, id.to_string (), json);
		if (!temp.attempt_password (txn_wallet, password))
		{
			error = store.import (txn_wallet, temp);
		}
		temp.destroy (txn_wallet);
	}
	catch (std::exception const & ex)
	{
		logger.error (nano::log::type::wallet, "Failed to import wallet: {}", ex.what ());
	}
	return error;
}

std::string nano::wallet::serialize_json ()
{
	auto transaction (wallets.tx_begin_read ());
	return store.serialize_json (transaction);
}

void nano::wallet::write_backup (std::filesystem::path const & path_a)
{
	auto transaction (wallets.tx_begin_read ());
	store.write_backup (transaction, path_a);
}

std::shared_ptr<nano::block> nano::wallet::receive_action (nano::block_hash const & send_hash_a, nano::account const & representative_a, nano::uint128_union const & amount_a, nano::account const & account_a, uint64_t work_a, bool generate_work_a)
{
	std::shared_ptr<nano::block> block;
	nano::block_details details;
	details.is_receive = true;
	if (wallets.config.receive_minimum.number () <= amount_a.number ())
	{
		auto ledger_txn = wallets.ledger.tx_begin_read ();
		auto transaction (wallets.tx_begin_read ());
		if (wallets.ledger.any.block_exists_or_pruned (ledger_txn, send_hash_a))
		{
			auto pending_info = wallets.ledger.any.pending_get (ledger_txn, nano::pending_key (account_a, send_hash_a));
			if (pending_info)
			{
				auto prv_result = store.fetch (transaction, account_a);
				if (prv_result)
				{
					logger.info (nano::log::type::wallet, "Receiving block: {} from account: {}, amount: {} raw",
					send_hash_a,
					account_a,
					nano::log::as_raw_nano (pending_info->amount));

					if (work_a == 0)
					{
						work_a = store.work_get (transaction, account_a).value_or (0);
					}
					auto info = wallets.ledger.any.account_get (ledger_txn, account_a);
					if (info)
					{
						block = std::make_shared<nano::state_block> (account_a, info->head, info->representative, info->balance.number () + pending_info->amount.number (), send_hash_a, prv_result.value (), account_a, work_a);
						details.epoch = std::max (info->epoch (), pending_info->epoch);
					}
					else
					{
						block = std::make_shared<nano::state_block> (account_a, 0, representative_a, pending_info->amount, reinterpret_cast<nano::link const &> (send_hash_a), prv_result.value (), account_a, work_a);
						details.epoch = pending_info->epoch;
					}
				}
				else
				{
					logger.warn (nano::log::type::wallet, "Unable to receive, wallet locked, block: {} to account: {}",
					send_hash_a,
					account_a);
				}
			}
			else
			{
				// Ledger doesn't have this marked as available to receive anymore
				logger.warn (nano::log::type::wallet, "Not receiving block: {}, block already received", send_hash_a);
			}
		}
		else
		{
			// Ledger doesn't have this block anymore.
			logger.warn (nano::log::type::wallet, "Not receiving block: {}, block no longer exists or pruned", send_hash_a);
		}
	}
	else
	{
		// Someone sent us something below the threshold of receiving
		logger.warn (nano::log::type::wallet, "Not receiving block: {} due to minimum receive threshold", send_hash_a);
	}
	if (block != nullptr)
	{
		if (action_complete (block, account_a, generate_work_a, details))
		{
			// Return null block after work generation or ledger process error
			block = nullptr;
		}
	}
	return block;
}

std::shared_ptr<nano::block> nano::wallet::change_action (nano::account const & source_a, nano::account const & representative_a, uint64_t work_a, bool generate_work_a)
{
	std::shared_ptr<nano::block> block;
	nano::block_details details;
	{
		auto transaction (wallets.tx_begin_read ());
		auto ledger_txn = wallets.ledger.tx_begin_read ();
		if (store.valid_password (transaction))
		{
			auto existing (store.find (transaction, source_a));
			if (existing != store.end (transaction) && !wallets.ledger.any.account_head (ledger_txn, source_a).is_zero ())
			{
				logger.info (nano::log::type::wallet, "Changing representative for account: {} to: {}",
				source_a,
				representative_a);

				auto info = wallets.ledger.any.account_get (ledger_txn, source_a);
				release_assert (info, "could not find account info for account in wallet change_action", source_a.to_account ());
				auto prv_result = store.fetch (transaction, source_a);
				release_assert (prv_result, "failed to fetch private key for account in wallet change_action", source_a.to_account ());
				if (work_a == 0)
				{
					work_a = store.work_get (transaction, source_a).value_or (0);
				}
				block = std::make_shared<nano::state_block> (source_a, info->head, representative_a, info->balance, 0, prv_result.value (), source_a, work_a);
				details.epoch = info->epoch ();
			}
			else
			{
				logger.warn (nano::log::type::wallet, "Changing representative for account: {} failed, wallet locked or account not found", source_a);
			}
		}
		else
		{
			logger.warn (nano::log::type::wallet, "Changing representative for account: {} failed, wallet locked", source_a);
		}
	}
	if (block != nullptr)
	{
		if (action_complete (block, source_a, generate_work_a, details))
		{
			// Return null block after work generation or ledger process error
			block = nullptr;
		}
	}
	return block;
}

std::shared_ptr<nano::block> nano::wallet::send_action (nano::account const & source_a, nano::account const & account_a, nano::uint128_t const & amount_a, uint64_t work_a, bool generate_work_a, boost::optional<std::string> id_a)
{
	boost::optional<nano::store::lmdb::db_val> id_mdb_val;
	if (id_a)
	{
		id_mdb_val = nano::store::lmdb::db_val (id_a->size (), const_cast<char *> (id_a->data ()));
	}

	auto prepare_send = [this, &id_mdb_val, &wallets = this->wallets, &store = this->store, &source_a, &amount_a, &work_a, &account_a, &id_a] (auto const & transaction) {
		auto ledger_txn = wallets.ledger.tx_begin_read ();
		auto error (false);
		auto cached_block (false);
		std::shared_ptr<nano::block> block;
		nano::block_details details;
		details.is_send = true;
		if (id_mdb_val)
		{
			nano::store::lmdb::db_val result;
			MDB_val mdb_id_key{ id_mdb_val->size (), id_mdb_val->data () };
			MDB_val mdb_result{};
			auto status (mdb_get (wallets.env.tx (transaction), wallets.send_action_ids, &mdb_id_key, &mdb_result));
			if (status == 0)
			{
				result = nano::store::lmdb::from_mdb_val (mdb_result);
				nano::block_hash hash (result);
				block = wallets.ledger.any.block_get (ledger_txn, hash);
				if (block != nullptr)
				{
					logger.warn (nano::log::type::wallet, "Block already exists for send action with id: {}, existing hash: {}",
					id_a.value (),
					hash);

					cached_block = true;
					wallets.network.flood_block (block, nano::transport::traffic_type::block_broadcast_initial);
				}
				else
				{
					logger.warn (nano::log::type::wallet, "Block was not found in ledger for send action with id: {}, hash: {}",
					id_a.value (),
					hash);
				}
			}
			else if (status != MDB_NOTFOUND)
			{
				error = true;
			}
		}
		if (!error && block == nullptr)
		{
			if (store.valid_password (transaction))
			{
				auto existing (store.find (transaction, source_a));
				if (existing != store.end (transaction))
				{
					auto balance (wallets.ledger.any.account_balance (ledger_txn, source_a));
					if (balance && balance.value ().number () >= amount_a)
					{
						logger.info (nano::log::type::wallet, "Sending from account: {} to: {}, amount: {} raw",
						source_a,
						account_a,
						nano::log::as_raw_nano (amount_a));

						auto info = wallets.ledger.any.account_get (ledger_txn, source_a);
						release_assert (info, "could not find account info for account in wallet send_action", source_a.to_account ());
						auto prv_result = store.fetch (transaction, source_a);
						release_assert (prv_result, "failed to fetch private key for account in wallet send_action", source_a.to_account ());
						if (work_a == 0)
						{
							work_a = store.work_get (transaction, source_a).value_or (0);
						}
						block = std::make_shared<nano::state_block> (source_a, info->head, info->representative, balance.value ().number () - amount_a, account_a, prv_result.value (), source_a, work_a);
						details.epoch = info->epoch ();
						if (id_mdb_val && block != nullptr)
						{
							nano::store::lmdb::db_val hash_val (block->hash ());
							auto mdb_id_key = nano::store::lmdb::to_mdb_val (*id_mdb_val);
							auto mdb_hash_val = nano::store::lmdb::to_mdb_val (hash_val);
							auto status (mdb_put (wallets.env.tx (transaction), wallets.send_action_ids, &mdb_id_key, &mdb_hash_val, 0));
							if (status != 0)
							{
								block = nullptr;
								error = true;
							}
						}
					}
					else
					{
						if (balance)
						{
							logger.warn (nano::log::type::wallet, "Insufficient balance for send from: {}, required: {} raw, available: {} raw",
							source_a,
							nano::log::as_raw_nano (amount_a),
							nano::log::as_raw_nano (balance.value ()));
						}
						else
						{
							logger.warn (nano::log::type::wallet, "Insufficient balance for send from: {}, required: {} raw, available: unknown",
							source_a,
							nano::log::as_raw_nano (amount_a));
						}
					}
				}
			}
		}
		return std::make_tuple (block, error, cached_block, details);
	};

	std::tuple<std::shared_ptr<nano::block>, bool, bool, nano::block_details> result;
	{
		if (id_mdb_val)
		{
			result = prepare_send (wallets.tx_begin_write ());
		}
		else
		{
			result = prepare_send (wallets.tx_begin_read ());
		}
	}

	std::shared_ptr<nano::block> block;
	bool error;
	bool cached_block;
	nano::block_details details;
	std::tie (block, error, cached_block, details) = result;

	if (!error && block != nullptr && !cached_block)
	{
		if (action_complete (block, source_a, generate_work_a, details))
		{
			// Return null block after work generation or ledger process error
			block = nullptr;
		}
	}
	return block;
}

bool nano::wallet::action_complete (std::shared_ptr<nano::block> const & block_a, nano::account const & account_a, bool const generate_work_a, nano::block_details const & details_a)
{
	bool error{ false };
	// Unschedule any work caching for this account
	wallets.delayed_work->erase (account_a);
	if (block_a != nullptr)
	{
		auto required_difficulty{ wallets.network_params.work.threshold (block_a->work_version (), details_a) };
		if (wallets.network_params.work.difficulty (*block_a) < required_difficulty)
		{
			logger.info (nano::log::type::wallet, "Cached or provided work for block: {}, account {}: is invalid, regenerating...",
			block_a->hash (),
			account_a);

			debug_assert (required_difficulty <= wallets.node.max_work_generate_difficulty (block_a->work_version ()));
			error = !wallets.node.work_generate_blocking (*block_a, required_difficulty).has_value ();
		}
		if (!error)
		{
			auto result = wallets.node.process_local (block_a);
			error = !result || result.value () != nano::block_status::progress;
			debug_assert (error || block_a->sideband ().details == details_a);
		}
		if (!error && generate_work_a)
		{
			// Pregenerate work for next block based on the block just created
			work_ensure (account_a, block_a->hash ());
		}
	}
	return error;
}

bool nano::wallet::change_sync (nano::account const & source_a, nano::account const & representative_a)
{
	std::promise<bool> result;
	std::future<bool> future = result.get_future ();
	change_async (
	source_a, representative_a, [&result] (std::shared_ptr<nano::block> const & block_a) {
		result.set_value (block_a == nullptr);
	},
	true);
	return future.get ();
}

void nano::wallet::change_async (nano::account const & source_a, nano::account const & representative_a, std::function<void (std::shared_ptr<nano::block> const &)> const & action_a, uint64_t work_a, bool generate_work_a)
{
	auto this_l (shared_from_this ());
	wallets.queue_wallet_action (nano::wallets::high_priority, this_l, [this_l, source_a, representative_a, action_a, work_a, generate_work_a] (nano::wallet & wallet_a) {
		auto block (wallet_a.change_action (source_a, representative_a, work_a, generate_work_a));
		action_a (block);
	});
}

bool nano::wallet::receive_sync (std::shared_ptr<nano::block> const & block_a, nano::account const & representative_a, nano::uint128_t const & amount_a)
{
	std::promise<bool> result;
	std::future<bool> future = result.get_future ();
	receive_async (
	block_a->hash (), representative_a, amount_a, block_a->destination (), [&result] (std::shared_ptr<nano::block> const & block_a) {
		result.set_value (block_a == nullptr);
	},
	true);
	return future.get ();
}

void nano::wallet::receive_async (nano::block_hash const & hash_a, nano::account const & representative_a, nano::uint128_t const & amount_a, nano::account const & account_a, std::function<void (std::shared_ptr<nano::block> const &)> const & action_a, uint64_t work_a, bool generate_work_a)
{
	auto this_l (shared_from_this ());
	wallets.queue_wallet_action (amount_a, this_l, [this_l, hash_a, representative_a, amount_a, account_a, action_a, work_a, generate_work_a] (nano::wallet & wallet_a) {
		auto block (wallet_a.receive_action (hash_a, representative_a, amount_a, account_a, work_a, generate_work_a));
		action_a (block);
	});
}

nano::block_hash nano::wallet::send_sync (nano::account const & source_a, nano::account const & account_a, nano::uint128_t const & amount_a)
{
	std::promise<nano::block_hash> result;
	std::future<nano::block_hash> future = result.get_future ();
	send_async (
	source_a, account_a, amount_a, [&result] (std::shared_ptr<nano::block> const & block_a) {
		result.set_value (block_a->hash ());
	},
	true);
	return future.get ();
}

void nano::wallet::send_async (nano::account const & source_a, nano::account const & account_a, nano::uint128_t const & amount_a, std::function<void (std::shared_ptr<nano::block> const &)> const & action_a, uint64_t work_a, bool generate_work_a, boost::optional<std::string> id_a)
{
	auto this_l (shared_from_this ());
	wallets.queue_wallet_action (nano::wallets::high_priority, this_l, [this_l, source_a, account_a, amount_a, action_a, work_a, generate_work_a, id_a] (nano::wallet & wallet_a) {
		auto block (wallet_a.send_action (source_a, account_a, amount_a, work_a, generate_work_a, id_a));
		action_a (block);
	});
}

// Update work for account if latest root is root_a
void nano::wallet::work_update_impl (nano::store::write_transaction const & transaction_a, nano::account const & account_a, nano::root const & root_a, uint64_t work_a)
{
	debug_assert (!wallets.network_params.work.validate_entry (nano::work_version::work_1, root_a, work_a));
	debug_assert (store.exists (transaction_a, account_a));
	auto ledger_txn = wallets.ledger.tx_begin_read ();
	auto latest (wallets.ledger.latest_root (ledger_txn, account_a));
	if (latest == root_a)
	{
		store.work_put (transaction_a, account_a, work_a);
	}
	else
	{
		logger.warn (nano::log::type::wallet, "Cached work no longer valid, discarding");
	}
}

void nano::wallet::work_ensure (nano::account const & account_a, nano::root const & root_a)
{
	using namespace std::chrono_literals;
	std::chrono::seconds const precache_delay = wallets.network_params.network.is_dev_network () ? 1s : 10s;

	wallets.delayed_work->operator[] (account_a) = root_a;

	wallets.workers.post_delayed (precache_delay, [this_l = shared_from_this (), account_a, root_a] {
		if (this_l->wallets.stopped)
		{
			return;
		}
		auto delayed_work = this_l->wallets.delayed_work.lock ();
		auto existing (delayed_work->find (account_a));
		if (existing != delayed_work->end () && existing->second == root_a)
		{
			delayed_work->erase (existing);
			this_l->wallets.queue_wallet_action (nano::wallets::generate_priority, this_l, [account_a, root_a] (nano::wallet & wallet_a) {
				wallet_a.work_cache_blocking (account_a, root_a);
			});
		}
	});
}

bool nano::wallet::search_receivable ()
{
	auto transaction = wallets.tx_begin_read ();
	return search_receivable_impl (transaction);
}

bool nano::wallet::search_receivable_impl (nano::store::transaction const & wallet_transaction_a)
{
	auto result (!store.valid_password (wallet_transaction_a));
	if (!result)
	{
		logger.debug (nano::log::type::wallet, "Beginning receivable block search");

		for (auto i (store.begin (wallet_transaction_a)), n (store.end (wallet_transaction_a)); i != n; ++i)
		{
			auto ledger_txn = wallets.ledger.tx_begin_read ();
			nano::account const & account (i->first);
			// Don't search pending for watch-only accounts
			if (!nano::wallet_value (i->second).key.is_zero ())
			{
				for (auto j (wallets.ledger.store.pending.begin (ledger_txn, nano::pending_key (account, 0))), k (wallets.ledger.store.pending.end (ledger_txn)); j != k && nano::pending_key (j->first).account == account; ++j)
				{
					nano::pending_key key (j->first);
					auto hash (key.hash);
					nano::pending_info pending (j->second);
					auto amount (pending.amount.number ());
					if (wallets.config.receive_minimum.number () <= amount)
					{
						bool const confirmed = wallets.ledger.cemented.block_exists_or_pruned (ledger_txn, hash);

						logger.info (nano::log::type::wallet, "Found a receivable block: {} ({}) for account: {} from: {}",
						hash,
						confirmed ? "confirmed" : "unconfirmed",
						key.account,
						pending.source);

						if (confirmed)
						{
							auto representative = store.representative (wallet_transaction_a);
							// Receive confirmed block
							receive_async (hash, representative, amount, account, [] (std::shared_ptr<nano::block> const &) {});
						}
						else if (!wallets.node.cementing_set.contains (hash))
						{
							auto block = wallets.ledger.any.block_get (ledger_txn, hash);
							if (block)
							{
								// Request confirmation for block which is not being processed yet
								wallets.node.start_election (block);
							}
						}
					}
				}
			}
		}

		logger.debug (nano::log::type::wallet, "Receivable block search phase complete");
	}
	else
	{
		logger.warn (nano::log::type::wallet, "Unable to search receivable blocks, wallet is locked. Blocks won't be auto-received until the wallet is unlocked");
	}
	return result;
}

void nano::wallet::init_free_accounts_impl (nano::store::transaction const & transaction_a)
{
	free_accounts.clear ();
	for (auto i (store.begin (transaction_a)), n (store.end (transaction_a)); i != n; ++i)
	{
		free_accounts.insert (i->first);
	}
}

uint32_t nano::wallet::deterministic_check (uint32_t index)
{
	auto transaction = wallets.tx_begin_read ();
	return deterministic_check_impl (transaction, index);
}

uint32_t nano::wallet::deterministic_check_impl (nano::store::transaction const & transaction_a, uint32_t index)
{
	auto ledger_txn = wallets.ledger.tx_begin_read ();
	for (uint32_t i (index + 1), n (index + 64); i < n; ++i)
	{
		auto prv = store.deterministic_key (transaction_a, i);
		nano::keypair pair (prv.to_string ());
		// Check if account received at least 1 block
		auto latest (wallets.ledger.any.account_head (ledger_txn, pair.pub));
		if (!latest.is_zero ())
		{
			index = i;
			// i + 64 - Check additional 64 accounts
			// i/64 - Check additional accounts for large wallets. I.e. 64000/64 = 1000 accounts to check
			n = i + 64 + (i / 64);
		}
		else
		{
			// Check if there are pending blocks for account
			auto current = wallets.ledger.any.receivable_upper_bound (ledger_txn, pair.pub, 0);
			if (current != wallets.ledger.any.receivable_end ())
			{
				index = i;
				n = i + 64 + (i / 64);
			}
		}
	}
	return index;
}

nano::public_key nano::wallet::change_seed (nano::raw_key const & prv_a, uint32_t count)
{
	nano::public_key result;
	{
		auto transaction = wallets.tx_begin_write ();
		result = change_seed_impl (transaction, prv_a, count);
	}
	wallets.refresh_rep_keys_cache ();
	return result;
}

nano::public_key nano::wallet::change_seed_impl (nano::store::write_transaction const & transaction_a, nano::raw_key const & prv_a, uint32_t count)
{
	logger.info (nano::log::type::wallet, "Changing wallet seed");

	store.seed_set (transaction_a, prv_a);
	auto account = deterministic_insert_impl (transaction_a);
	if (count == 0)
	{
		count = deterministic_check_impl (transaction_a, 0);
		logger.info (nano::log::type::wallet, "Auto-detected {} accounts to generate from seed", count);
	}
	for (uint32_t i (0); i < count; ++i)
	{
		// Disable work generation to prevent weak CPU nodes stuck
		account = deterministic_insert_impl (transaction_a, false);
	}

	logger.info (nano::log::type::wallet, "Completed changing wallet seed and generating accounts");

	return account;
}

void nano::wallet::deterministic_restore ()
{
	{
		auto transaction = wallets.tx_begin_write ();
		deterministic_restore_impl (transaction);
	}
	wallets.refresh_rep_keys_cache ();
}

void nano::wallet::deterministic_restore_impl (nano::store::write_transaction const & transaction_a)
{
	auto index (store.deterministic_index_get (transaction_a));
	auto new_index (deterministic_check_impl (transaction_a, index));
	for (uint32_t i (index); i <= new_index && index != new_index; ++i)
	{
		// Disable work generation to prevent weak CPU nodes stuck
		deterministic_insert_impl (transaction_a, false);
	}
}

bool nano::wallet::rekey (std::string const & password_a)
{
	bool result;
	{
		auto transaction = wallets.tx_begin_write ();
		result = store.rekey (transaction, password_a);
	}
	if (!result)
	{
		wallets.refresh_rep_keys_cache ();
	}
	return result;
}

bool nano::wallet::is_locked () const
{
	auto transaction = wallets.tx_begin_read ();
	return !store.valid_password (transaction);
}

void nano::wallet::lock ()
{
	logger.info (nano::log::type::wallet, "Wallet locked");
	nano::raw_key empty;
	empty.clear ();
	store.password.value_set (empty);
	wallets.refresh_rep_keys_cache ();
}

void nano::wallet::remove_account (nano::account const & account_a)
{
	{
		auto transaction = wallets.tx_begin_write ();
		store.erase (transaction, account_a);
	}
	wallets.refresh_rep_keys_cache ();
}

std::vector<nano::account> nano::wallet::accounts () const
{
	auto transaction = wallets.tx_begin_read ();
	return store.accounts (transaction);
}

bool nano::wallet::move_accounts (wallet & source, std::vector<nano::public_key> const & accounts_a)
{
	bool error;
	{
		auto transaction = wallets.tx_begin_write ();
		error = store.move (transaction, source.store, accounts_a);
	}
	wallets.refresh_rep_keys_cache ();
	return error;
}

nano::key_type nano::wallet::key_type (nano::account const & account) const
{
	auto txn_wallet = wallets.tx_begin_read ();
	return store.key_type (txn_wallet, account);
}

void nano::wallet::set_representative (nano::account const & rep_a)
{
	auto transaction = wallets.tx_begin_write ();
	store.representative_set (transaction, rep_a);
}

nano::account nano::wallet::get_representative () const
{
	auto transaction = wallets.tx_begin_read ();
	return store.representative (transaction);
}

nano::result<nano::raw_key> nano::wallet::get_seed () const
{
	auto transaction = wallets.tx_begin_read ();
	if (!store.valid_password (transaction))
	{
		return nano::error (nano::error_common::wallet_locked);
	}
	return store.seed (transaction);
}

uint32_t nano::wallet::get_deterministic_index () const
{
	auto transaction = wallets.tx_begin_read ();
	return store.deterministic_index_get (transaction);
}

nano::result<uint64_t> nano::wallet::get_work (nano::public_key const & pub) const
{
	auto transaction = wallets.tx_begin_read ();
	auto result = store.work_get (transaction, pub);
	if (result)
	{
		return *result;
	}
	return nano::error (nano::error_common::account_not_found_wallet);
}

void nano::wallet::set_work (nano::public_key const & pub_a, uint64_t work_a)
{
	auto transaction = wallets.tx_begin_write ();
	store.work_put (transaction, pub_a, work_a);
}

nano::result<nano::raw_key> nano::wallet::fetch_prv (nano::account const & pub_a) const
{
	auto transaction = wallets.tx_begin_read ();
	return store.fetch (transaction, pub_a);
}

bool nano::wallet::is_open ()
{
	return store.is_open ();
}

std::unordered_set<nano::account> nano::wallet::reps () const
{
	return *representatives.lock ();
}

void nano::wallet::work_cache_blocking (nano::account const & account_a, nano::root const & root_a)
{
	if (wallets.node.work_generation_enabled ())
	{
		auto difficulty (wallets.node.default_difficulty (nano::work_version::work_1));
		auto opt_work_l (wallets.node.work_generate_blocking (nano::work_version::work_1, root_a, difficulty, account_a));
		if (opt_work_l.has_value ())
		{
			auto transaction_l (wallets.tx_begin_write ());
			if (is_open () && store.exists (transaction_l, account_a))
			{
				work_update_impl (transaction_l, account_a, root_a, opt_work_l.value ());
			}
		}
		else if (!wallets.node.stopped)
		{
			logger.warn (nano::log::type::wallet, "Could not precache work for root: {} due to work generation failure", root_a);
		}
	}
}

/*
 * wallets
 */

nano::uint128_t const nano::wallets::generate_priority = std::numeric_limits<nano::uint128_t>::max ();
nano::uint128_t const nano::wallets::high_priority = std::numeric_limits<nano::uint128_t>::max () - 1;

nano::wallets::wallets (
nano::node & node_a,
nano::wallets_store & wallets_store_a,
nano::ledger & ledger_a,
nano::node_config const & config_a,
nano::network_params const & network_params_a,
nano::online_reps & online_reps_a,
nano::network & network_a,
nano::stats & stats_a,
nano::logger & logger_a) :
	node{ node_a },
	wallets_store{ wallets_store_a },
	ledger{ ledger_a },
	config{ config_a },
	network_params{ network_params_a },
	online_reps{ online_reps_a },
	network{ network_a },
	stats{ stats_a },
	logger{ logger_a },
	observer{ [] (bool) {} },
	kdf{ network_params.kdf_work },
	env{ boost::polymorphic_downcast<nano::mdb_wallets_store *> (&wallets_store)->environment },
	workers{ config.wallet_threads, nano::thread_role::name::wallet_worker, /* auto_start */ true }
{
	logger.info (nano::log::type::wallet, "Loading wallets from: {}", env.database_path.string ());

	nano::unique_lock<nano::mutex> lock{ mutex };
	{
		auto transaction (tx_begin_write ());
		auto status (mdb_dbi_open (env.tx (transaction), nullptr, MDB_CREATE, &handle));
		status |= mdb_dbi_open (env.tx (transaction), "send_action_ids", MDB_CREATE, &send_action_ids);
		release_assert (nano::store::lmdb::success (status), nano::store::lmdb::error_string (status));
		std::string beginning (nano::uint256_union (0).to_string ());
		nano::store::lmdb::db_val beginning_val{ beginning.size (), const_cast<char *> (beginning.c_str ()) };
		std::string end ((nano::uint256_union (nano::uint256_t (0) - nano::uint256_t (1))).to_string ());
		nano::store::lmdb::db_val end_val{ end.size (), const_cast<char *> (end.c_str ()) };
		auto mdb_beginning_val = nano::store::lmdb::to_mdb_val (beginning_val);
		auto mdb_end_val = nano::store::lmdb::to_mdb_val (end_val);
		store::iterator i{ transaction, store::lmdb::iterator::lower_bound (env.tx (transaction), handle, mdb_beginning_val) };
		store::iterator n{ transaction, store::lmdb::iterator::lower_bound (env.tx (transaction), handle, mdb_end_val) };
		for (; i != n; ++i)
		{
			nano::wallet_id id;
			std::string text (reinterpret_cast<char const *> (i->first.data ()), i->first.size ());
			auto error (id.decode_hex (text));
			release_assert (!error, "failed to decode wallet id from text: {}", text);
			release_assert (items.find (id) == items.end ());
			try
			{
				auto wallet = nano::wallet::open_existing (transaction, *this, text);
				items[id] = wallet;
			}
			catch (std::exception const & ex)
			{
				logger.error (nano::log::type::wallet, "Failed to open wallet {}: {}", text, ex.what ());
			}
		}
	}

	logger.info (nano::log::type::wallet, "Found {} wallet(s)", items.size ());
	for (auto const & item : items)
	{
		logger.info (nano::log::type::wallet, "Wallet: {}", item.first);
	}

	// Backup before upgrade wallets
	bool backup_required (false);
	if (config.backup_before_upgrade)
	{
		auto transaction (tx_begin_read ());
		for (auto & item : items)
		{
			if (item.second->store.version (transaction) != nano::wallet_store::version_current)
			{
				backup_required = true;
				break;
			}
		}
	}
	if (backup_required)
	{
		char const * store_path;
		mdb_env_get_path (env, &store_path);
		std::filesystem::path const path (store_path);
		env.create_backup_file (path, logger);
	}
	for (auto & item : items)
	{
		item.second->enter_initial_password ();
	}
}

nano::wallets::~wallets ()
{
	stop ();
}

void nano::wallets::start ()
{
	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::wallet_actions);
		do_wallet_actions ();
	} };

	if (config.enable_voting)
	{
		reps_thread = std::thread{ [this] () {
			nano::thread_role::set (nano::thread_role::name::wallet_reps);
			run_reps_scan ();
		} };
	}

	if (!node.flags.disable_search_pending)
	{
		receivable_thread = std::thread{ [this] () {
			nano::thread_role::set (nano::thread_role::name::wallet_receivable);
			run_receivable_scan ();
		} };
	}
}

void nano::wallets::stop ()
{
	{
		nano::lock_guard<nano::mutex> action_lock{ action_mutex };
		stopped = true;
		actions.clear ();
	}
	condition.notify_all ();
	reps_condition.notify_all ();
	receivable_condition.notify_all ();

	if (thread.joinable ())
	{
		thread.join ();
	}
	if (reps_thread.joinable ())
	{
		reps_thread.join ();
	}
	if (receivable_thread.joinable ())
	{
		receivable_thread.join ();
	}

	workers.stop ();
}

std::shared_ptr<nano::wallet> nano::wallets::open (nano::wallet_id const & id_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	std::shared_ptr<nano::wallet> result;
	auto existing (items.find (id_a));
	if (existing != items.end ())
	{
		result = existing->second;
	}
	return result;
}

std::shared_ptr<nano::wallet> nano::wallets::create (nano::wallet_id const & id_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	debug_assert (items.find (id_a) == items.end ());
	{
		auto txn_wallet = tx_begin_write ();
		try
		{
			auto result = nano::wallet::create (txn_wallet, *this, id_a.to_string (), config.random_representative ());
			debug_assert (result->store.valid_password (txn_wallet));
			items[id_a] = result;
			return result;
		}
		catch (std::exception const & ex)
		{
			logger.error (nano::log::type::wallet, "Failed to create wallet {}: {}", id_a, ex.what ());
		}
	}
	return nullptr;
}

std::shared_ptr<nano::wallet> nano::wallets::create_from_json (nano::wallet_id const & id_a, std::string const & json)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	debug_assert (items.find (id_a) == items.end ());
	{
		auto txn_wallet = tx_begin_write ();
		try
		{
			auto result = nano::wallet::create_from_json (txn_wallet, *this, id_a.to_string (), json);
			items[id_a] = result;
			return result;
		}
		catch (std::exception const & ex)
		{
			logger.error (nano::log::type::wallet, "Failed to create wallet {} from JSON: {}", id_a, ex.what ());
		}
	}
	return nullptr;
}

bool nano::wallets::search_receivable (nano::wallet_id const & wallet_a)
{
	auto result (false);
	if (auto wallet = open (wallet_a); wallet != nullptr)
	{
		result = wallet->search_receivable ();
	}
	return result;
}

void nano::wallets::search_receivable_all ()
{
	auto wallets_l = all_wallets ();
	for (auto const & [id, wallet] : wallets_l)
	{
		wallet->search_receivable ();
	}
}

void nano::wallets::destroy (nano::wallet_id const & id_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto transaction (tx_begin_write ());
	// action_mutex should be after transactions to prevent deadlocks in deterministic_insert () & insert_adhoc ()
	nano::lock_guard<nano::mutex> action_lock{ action_mutex };
	auto existing (items.find (id_a));
	release_assert (existing != items.end ());
	auto wallet (existing->second);
	items.erase (existing);
	wallet->store.destroy (transaction);
}

void nano::wallets::reload ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto transaction (tx_begin_write ());
	std::unordered_set<nano::uint256_union> stored_items;
	std::string beginning (nano::uint256_union (0).to_string ());
	nano::store::lmdb::db_val beginning_val{ beginning.size (), const_cast<char *> (beginning.c_str ()) };
	std::string end ((nano::uint256_union (nano::uint256_t (0) - nano::uint256_t (1))).to_string ());
	nano::store::lmdb::db_val end_val{ end.size (), const_cast<char *> (end.c_str ()) };
	auto mdb_beginning_val = nano::store::lmdb::to_mdb_val (beginning_val);
	auto mdb_end_val = nano::store::lmdb::to_mdb_val (end_val);
	store::iterator i{ transaction, store::lmdb::iterator::lower_bound (env.tx (transaction), handle, mdb_beginning_val) };
	store::iterator n{ transaction, store::lmdb::iterator::lower_bound (env.tx (transaction), handle, mdb_end_val) };
	for (; i != n; ++i)
	{
		nano::wallet_id id;
		std::string text (reinterpret_cast<char const *> (i->first.data ()), i->first.size ());
		auto error (id.decode_hex (text));
		release_assert (!error, "error decoding wallet id from text", text);
		// New wallet
		if (items.find (id) == items.end ())
		{
			try
			{
				auto wallet = nano::wallet::open_existing (transaction, *this, text);
				items[id] = wallet;
			}
			catch (std::exception const & ex)
			{
				logger.error (nano::log::type::wallet, "Failed to open wallet {}: {}", text, ex.what ());
			}
		}
		// List of wallets on disk
		stored_items.insert (id);
	}
	// Delete non existing wallets from memory
	std::vector<nano::wallet_id> deleted_items;
	for (auto i : items)
	{
		if (stored_items.find (i.first) == stored_items.end ())
		{
			deleted_items.push_back (i.first);
		}
	}
	for (auto & i : deleted_items)
	{
		debug_assert (items.find (i) == items.end ());
		items.erase (i);
	}
}

void nano::wallets::queue_wallet_action (nano::uint128_t const & amount_a, std::shared_ptr<nano::wallet> const & wallet_a, std::function<void (nano::wallet &)> action_a)
{
	{
		nano::lock_guard<nano::mutex> action_lock{ action_mutex };
		actions.emplace (amount_a, std::make_pair (wallet_a, action_a));
	}
	condition.notify_all ();
}

bool nano::wallets::exists_impl (nano::store::transaction const & transaction_a, nano::account const & account_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto result (false);
	for (auto i (items.begin ()), n (items.end ()); !result && i != n; ++i)
	{
		result = i->second->store.exists (transaction_a, account_a);
	}
	return result;
}

bool nano::wallets::exists (nano::account const & account_a)
{
	auto transaction (tx_begin_read ());
	return exists_impl (transaction, account_a);
}

bool nano::wallets::exists_any (nano::account const & account1, nano::account const & account2)
{
	auto transaction (tx_begin_read ());
	return exists_impl (transaction, account1) || exists_impl (transaction, account2);
}

nano::store::write_transaction nano::wallets::tx_begin_write ()
{
	return env.tx_begin_write ();
}

nano::store::read_transaction nano::wallets::tx_begin_read ()
{
	return env.tx_begin_read ();
}

void nano::wallets::clear_send_ids ()
{
	auto transaction (tx_begin_write ());
	auto status (mdb_drop (env.tx (transaction), send_action_ids, 0));
	release_assert (nano::store::lmdb::success (status), nano::store::lmdb::error_string (status));
}

nano::wallet_representatives nano::wallets::reps () const
{
	return *representatives.lock ();
}

auto nano::wallets::signer () -> signer_t
{
	return [this] (auto const & callback) { foreach_representative (callback); };
}

bool nano::wallets::check_rep (nano::account const & account)
{
	auto half_principal_weight = node.minimum_principal_weight () / 2;
	auto representatives_locked = representatives.lock ();
	return check_rep_impl (*representatives_locked, account, half_principal_weight);
}

bool nano::wallets::check_rep_impl (wallet_representatives & reps, nano::account const & account, nano::uint128_t const & half_principal_weight)
{
	auto weight = ledger.weight (account);
	if (weight < config.vote_minimum.number ())
	{
		return false; // account not a representative
	}

	if (weight >= half_principal_weight)
	{
		reps.half_principal = true;
	}

	auto insert_result = reps.accounts.insert (account);
	if (!insert_result.second)
	{
		return false; // account already exists
	}

	++reps.voting;

	return true;
}

void nano::wallets::refresh_reps ()
{
	refresh_rep_index ();
	refresh_rep_keys_cache ();
}

void nano::wallets::refresh_rep_index ()
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	auto reps_locked = representatives.lock ();
	reps_locked->clear ();

	auto const half_principal_weight = node.minimum_principal_weight () / 2;

	auto wallet_txn = tx_begin_read ();

	for (auto const & [id, wallet] : items)
	{
		std::unordered_set<nano::account> new_representatives;
		for (auto i = wallet->store.begin (wallet_txn), n = wallet->store.end (wallet_txn); i != n; ++i)
		{
			auto account = i->first;
			if (check_rep_impl (*reps_locked, account, half_principal_weight))
			{
				new_representatives.insert (account);
			}
		}
		wallet->representatives.lock ()->swap (new_representatives);
	}
}

void nano::wallets::foreach_representative (std::function<void (nano::public_key const & pub, nano::raw_key const & prv)> const & action)
{
	if (config.enable_voting)
	{
		// Cache lock is held during callbacks, recursive calls are not allowed
		auto locked = rep_keys_cache.lock ();
		for (auto const & [pub, fan_ptr] : *locked)
		{
			nano::raw_key prv;
			fan_ptr->value (prv);
			action (pub, prv);
		}
	}
}

void nano::wallets::refresh_rep_keys_cache ()
{
	if (!config.enable_voting)
	{
		return;
	}

	std::vector<std::pair<nano::public_key, std::unique_ptr<nano::fan>>> new_cache;

	auto wallet_txn = tx_begin_read ();

	nano::lock_guard<nano::mutex> lock{ mutex };

	for (auto const & [id, wallet] : items)
	{
		nano::lock_guard<std::recursive_mutex> store_lock{ wallet->store.mutex };

		auto reps_locked = wallet->representatives.lock ();
		for (auto const & account : *reps_locked)
		{
			if (wallet->store.exists (wallet_txn, account))
			{
				if (wallet->store.valid_password (wallet_txn))
				{
					auto prv_result = wallet->store.fetch (wallet_txn, account);
					release_assert (prv_result, "failed to fetch private key for representative account", account.to_account ());

					// Store private key spread across multiple heap allocations via fan to avoid plaintext keys in memory at rest
					new_cache.emplace_back (account, std::make_unique<nano::fan> (prv_result.value (), config.password_fanout));
				}
				else
				{
					static auto last_log = std::chrono::steady_clock::time_point ();
					if (last_log < std::chrono::steady_clock::now () - std::chrono::seconds (60))
					{
						last_log = std::chrono::steady_clock::now ();
						logger.warn (nano::log::type::wallet, "Representative locked inside wallet: {}", id);
					}
				}
			}
		}
	}
	rep_keys_cache.lock ()->swap (new_cache);
}

void nano::wallets::run_reps_scan ()
{
	auto delay = [this] () {
		// Representation drifts quickly on the test network but very slowly on the live network
		return network_params.network.is_dev_network ()
		? 100ms
		: (network_params.network.is_test_network ()
		? std::chrono::milliseconds (nano::test_scan_wallet_reps_delay ())
		: std::chrono::minutes (15));
	};

	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		lock.unlock ();

		stats.inc (nano::stat::type::wallet, nano::stat::detail::loop_reps);

		// Recompute local wallet representatives and refresh cached keys
		refresh_reps ();

		lock.lock ();

		reps_condition.wait_for (lock, delay (), [this] () {
			return stopped.load ();
		});
	}
}

void nano::wallets::run_receivable_scan ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		lock.unlock ();

		stats.inc (nano::stat::type::wallet, nano::stat::detail::loop_receivable);

		// Reload wallets from disk
		reload ();

		// Search pending
		search_receivable_all ();

		lock.lock ();

		receivable_condition.wait_for (lock, network_params.node.search_pending_interval, [this] () {
			return stopped.load ();
		});
	}
}

void nano::wallets::receive_confirmed (nano::block_hash const & hash_a, nano::account const & destination_a)
{
	auto wallets_l = all_wallets ();
	auto wallet_transaction = tx_begin_read ();
	for ([[maybe_unused]] auto const & [id, wallet] : wallets_l)
	{
		if (wallet->store.exists (wallet_transaction, destination_a))
		{
			nano::account representative;
			representative = wallet->store.representative (wallet_transaction);
			auto pending = ledger.any.pending_get (ledger.tx_begin_read (), nano::pending_key (destination_a, hash_a));
			if (pending)
			{
				auto amount (pending->amount.number ());
				wallet->receive_async (hash_a, representative, amount, destination_a, [] (std::shared_ptr<nano::block> const &) {});
			}
			else
			{
				if (!ledger.cemented.block_exists_or_pruned (ledger.tx_begin_read (), hash_a))
				{
					logger.warn (nano::log::type::wallet, "Confirmed block is missing: {}", hash_a);
					debug_assert (false, "confirmed block is missing");
				}
				else
				{
					logger.warn (nano::log::type::wallet, "Block has already been received: {}", hash_a);
				}
			}
		}
	}
}

std::unordered_map<nano::wallet_id, std::shared_ptr<nano::wallet>> nano::wallets::all_wallets ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return items;
}

void nano::wallets::do_wallet_actions ()
{
	nano::unique_lock<nano::mutex> action_lock{ action_mutex };
	while (!stopped)
	{
		if (!actions.empty ())
		{
			auto first (actions.begin ());
			auto wallet (first->second.first);
			auto current (std::move (first->second.second));
			actions.erase (first);
			if (wallet->is_open ())
			{
				action_lock.unlock ();
				observer (true);
				current (*wallet);
				observer (false);
				action_lock.lock ();
			}
		}
		else
		{
			condition.wait (action_lock);
		}
	}
}

nano::container_info nano::wallets::container_info () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	nano::container_info info;
	info.put ("items", items.size ());
	info.put ("actions", actions.size ());
	info.put ("rep_keys_cache", rep_keys_cache.lock ()->size ());
	return info;
}

/*
 * fan
 */

nano::fan::fan (nano::raw_key const & key, std::size_t count_a)
{
	auto first (std::make_unique<nano::raw_key> (key));
	for (auto i (1); i < count_a; ++i)
	{
		auto entry (std::make_unique<nano::raw_key> ());
		nano::random_pool::generate_block (entry->bytes.data (), entry->bytes.size ());
		*first ^= *entry;
		values.push_back (std::move (entry));
	}
	values.push_back (std::move (first));
}

void nano::fan::value (nano::raw_key & prv_a) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	value_get (prv_a);
}

void nano::fan::value_get (nano::raw_key & prv_a) const
{
	debug_assert (!mutex.try_lock ());
	prv_a.clear ();
	for (auto & i : values)
	{
		prv_a ^= *i;
	}
}

void nano::fan::value_set (nano::raw_key const & value_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	nano::raw_key value_l;
	value_get (value_l);
	*(values[0]) ^= value_l;
	*(values[0]) ^= value_a;
}

/*
 * kdf
 */

void nano::kdf::phs (nano::raw_key & result_a, std::string const & password_a, nano::uint256_union const & salt_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto success (argon2_hash (1, kdf_work, 1, password_a.data (), password_a.size (), salt_a.bytes.data (), salt_a.bytes.size (), result_a.bytes.data (), result_a.bytes.size (), NULL, 0, Argon2_d, 0x10));
	release_assert (success == 0);
	(void)success;
}
