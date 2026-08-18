#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/files.hpp>
#include <nano/lib/formatting.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/threading.hpp>
#include <nano/lib/utility.hpp>
#include <nano/lib/work_version.hpp>
#include <nano/node/cementing_set.hpp>
#include <nano/node/election.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/traffic_type.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_cemented.hpp>
#include <nano/store/ledger/pending.hpp>
#include <nano/store/typed_iterator_templ.hpp>
#include <nano/wallet/wallet_value.hpp>
#include <nano/wallet/wallets_backend.hpp>

#include <boost/format.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <future>
#include <stdexcept>
#include <type_traits>

namespace nano::wallet
{
/*
 * wallet_data
 */

wallet_data::wallet_data (nano::store::write_transaction & transaction_a, nano::wallet::wallets & wallets_a, nano::wallet_id const & id_a) :
	id{ id_a },
	store{ wallets_a.kdf, transaction_a, wallets_a.backend, wallets_a.config.random_representative (), wallets_a.config.password_fanout, id_a.to_string () },
	handle{ std::make_shared<wallet> (wallets_a, id_a) }
{
}

wallet_data::wallet_data (nano::store::write_transaction & transaction_a, nano::wallet::wallets & wallets_a, nano::wallet_id const & id_a, std::string const & json) :
	id{ id_a },
	store{ wallets_a.kdf, transaction_a, wallets_a.backend, wallets_a.config.password_fanout, id_a.to_string (), json },
	handle{ std::make_shared<wallet> (wallets_a, id_a) }
{
}

/*
 * wallet
 */

wallet::wallet (nano::wallet::wallets & wallets_a, nano::wallet_id const & id_a) :
	wallets{ wallets_a },
	id{ id_a }
{
}

void wallet::enter_initial_password ()
{
	wallets.enter_initial_password (id);
}

bool wallet::enter_password (std::string const & password)
{
	return wallets.enter_password (id, password);
}

bool wallet::rekey (std::string const & password)
{
	return wallets.rekey (id, password);
}

bool wallet::is_locked () const
{
	return wallets.is_locked (id);
}

void wallet::lock ()
{
	wallets.lock (id);
}

void wallet::set_lock_observer (std::function<void (bool, bool)> observer)
{
	wallets.set_lock_observer (id, std::move (observer));
}

nano::result<nano::public_key> wallet::insert_adhoc (nano::raw_key const & prv, bool generate_work)
{
	return wallets.insert_adhoc (id, prv, generate_work);
}

nano::result<nano::public_key> wallet::deterministic_insert (uint32_t index, bool generate_work)
{
	return wallets.deterministic_insert (id, index, generate_work);
}

nano::result<nano::public_key> wallet::deterministic_insert (bool generate_work)
{
	return wallets.deterministic_insert (id, generate_work);
}

bool wallet::insert_watch (nano::public_key const & pub)
{
	return wallets.insert_watch (id, pub);
}

void wallet::remove_account (nano::account const & account)
{
	wallets.remove_account (id, account);
}

std::vector<nano::account> wallet::accounts () const
{
	return wallets.accounts (id);
}

bool wallet::exists (nano::public_key const & account) const
{
	return wallets.exists (id, account);
}

nano::result<bool> wallet::move_accounts (wallet & source, std::vector<nano::public_key> const & accounts)
{
	return wallets.move_accounts (id, source.id, accounts);
}

key_type wallet::key_type (nano::account const & account) const
{
	return wallets.key_type (id, account);
}

nano::result<nano::raw_key> wallet::get_seed () const
{
	return wallets.get_seed (id);
}

nano::result<nano::public_key> wallet::change_seed (nano::raw_key const & seed, uint32_t count)
{
	return wallets.change_seed (id, seed, count);
}

void wallet::deterministic_restore ()
{
	wallets.deterministic_restore (id);
}

std::optional<uint32_t> wallet::deterministic_check (uint32_t index) const
{
	return wallets.deterministic_check (id, index);
}

uint32_t wallet::get_deterministic_index () const
{
	return wallets.get_deterministic_index (id);
}

void wallet::set_representative (nano::account const & rep)
{
	wallets.set_representative (id, rep);
}

nano::account wallet::get_representative () const
{
	return wallets.get_representative (id);
}

std::unordered_set<nano::account> wallet::reps () const
{
	return wallets.reps (id);
}

nano::result<nano::raw_key> wallet::fetch_prv (nano::account const & pub) const
{
	return wallets.fetch_prv (id, pub);
}

std::shared_ptr<nano::block> wallet::change_action (nano::account const & source, nano::account const & representative, uint64_t work, bool generate_work)
{
	return wallets.change_action (id, source, representative, work, generate_work);
}

std::shared_ptr<nano::block> wallet::receive_action (nano::block_hash const & send_hash, nano::account const & representative, nano::uint128_union const & amount, nano::account const & account, uint64_t work, bool generate_work)
{
	return wallets.receive_action (id, send_hash, representative, amount, account, work, generate_work);
}

std::shared_ptr<nano::block> wallet::send_action (nano::account const & source, nano::account const & account, nano::uint128_t const & amount, uint64_t work, bool generate_work, std::optional<std::string> send_id)
{
	return wallets.send_action (id, source, account, amount, work, generate_work, send_id);
}

bool wallet::change_sync (nano::account const & source, nano::account const & representative)
{
	return wallets.change_sync (id, source, representative);
}

void wallet::change_async (nano::account const & source, nano::account const & representative, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work, bool generate_work)
{
	wallets.change_async (id, source, representative, action, work, generate_work);
}

bool wallet::receive_sync (std::shared_ptr<nano::block> const & block, nano::account const & representative, nano::uint128_t const & amount)
{
	return wallets.receive_sync (id, block, representative, amount);
}

void wallet::receive_async (nano::block_hash const & hash, nano::account const & representative, nano::uint128_t const & amount, nano::account const & account, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work, bool generate_work)
{
	wallets.receive_async (id, hash, representative, amount, account, action, work, generate_work);
}

nano::block_hash wallet::send_sync (nano::account const & source, nano::account const & account, nano::uint128_t const & amount)
{
	return wallets.send_sync (id, source, account, amount);
}

void wallet::send_async (nano::account const & source, nano::account const & account, nano::uint128_t const & amount, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work, bool generate_work, std::optional<std::string> send_id)
{
	wallets.send_async (id, source, account, amount, action, work, generate_work, send_id);
}

void wallet::work_cache_blocking (nano::account const & account, nano::root const & root)
{
	wallets.work_cache_blocking (id, account, root);
}

void wallet::work_ensure (nano::account const & account, nano::root const & root)
{
	wallets.work_ensure (id, account, root);
}

nano::result<uint64_t> wallet::get_work (nano::public_key const & pub) const
{
	return wallets.get_work (id, pub);
}

void wallet::set_work (nano::public_key const & pub, uint64_t work)
{
	wallets.set_work (id, pub, work);
}

bool wallet::search_receivable ()
{
	return wallets.search_receivable (id);
}

bool wallet::import (std::string const & json, std::string const & password)
{
	return wallets.import (id, json, password);
}

void wallet::serialize_json (std::string & json) const
{
	wallets.serialize_json (id, json);
}

void wallet::write_backup (std::filesystem::path const & path) const
{
	wallets.write_backup (id, path);
}

nano::fan & wallet::password_fan ()
{
	return wallets.password_fan (id);
}

/*
 * wallets
 */

wallets::wallets (
nano::node & node_a,
nano::wallet::wallets_backend & backend_a,
nano::ledger & ledger_a,
nano::node_config const & config_a,
nano::network_params const & network_params_a,
nano::online_reps & online_reps_a,
nano::network & network_a,
nano::stats & stats_a,
nano::logger & logger_a) :
	node{ node_a },
	backend{ backend_a },
	ledger{ ledger_a },
	config{ config_a },
	network_params{ network_params_a },
	online_reps{ online_reps_a },
	network{ network_a },
	stats{ stats_a },
	logger{ logger_a },
	kdf{ network_params.kdf_work },
	rep_tracker{ *this, node, ledger, config, network_params, stats, logger },
	receivable_tracker{ *this, node, ledger, config, network_params, stats, logger },
	action_runner{ *this, node, config, network_params, network, logger }
{
	logger.info (nano::log::type::wallet, "Loading wallets from: {}", backend.database_path ().string ());

	// No locking: single-threaded until start ()
	{
		auto transaction = tx_begin_write ();
		for (auto it = backend.index_begin (transaction), end = backend.index_end (transaction); it != end; ++it)
		{
			// The wallet index range may also include entries for non-wallet sub-tables (e.g. `send_action_ids` on LMDB);
			// skip anything that doesn't parse as a 64-char hex wallet id.
			auto id = try_parse_wallet_id (bytes_to_string (it->first));
			if (!id)
			{
				continue;
			}
			release_assert (items.find (*id) == items.end ());
			try
			{
				items[*id] = std::make_unique<wallet_data> (transaction, *this, *id);
			}
			catch (std::exception const & ex)
			{
				logger.error (nano::log::type::wallet, "Failed to open wallet {}: {}", *id, ex.what ());
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
		auto transaction = tx_begin_read ();
		for (auto & item : items)
		{
			if (item.second->store.version (transaction) != wallet_store::version_current)
			{
				backup_required = true;
				break;
			}
		}
	}
	if (backup_required)
	{
		backend.backup (logger);
	}
	for (auto const & [id, wallet_l] : items)
	{
		enter_initial_password (id);
	}
}

wallets::~wallets ()
{
	stop ();
}

void wallets::start ()
{
	action_runner.start ();
	rep_tracker.start ();
	receivable_tracker.start ();
}

void wallets::stop ()
{
	receivable_tracker.stop ();
	action_runner.stop ();
	rep_tracker.stop ();
}

std::shared_ptr<wallet> wallets::open (nano::wallet_id const & id)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	return wallet_l != nullptr ? wallet_l->handle : nullptr;
}

std::shared_ptr<wallet> wallets::create (nano::wallet_id const & id)
{
	// Write transactions are always acquired before the mutex so commit fsyncs never happen inside the critical section
	auto transaction = tx_begin_write ();
	nano::lock_guard<nano::mutex> lock{ mutex };
	debug_assert (items.find (id) == items.end ());
	try
	{
		auto wallet_l = std::make_unique<wallet_data> (transaction, *this, id);
		debug_assert (wallet_l->store.valid_password (transaction));
		auto handle = wallet_l->handle;
		// Commit before the entry becomes visible so readers never observe an uncommitted store
		transaction.commit ();
		items[id] = std::move (wallet_l);
		return handle;
	}
	catch (std::exception const & ex)
	{
		logger.error (nano::log::type::wallet, "Failed to create wallet {}: {}", id, ex.what ());
	}
	return nullptr;
}

std::shared_ptr<wallet> wallets::create_from_json (nano::wallet_id const & id, std::string const & json)
{
	auto transaction = tx_begin_write ();
	nano::lock_guard<nano::mutex> lock{ mutex };
	debug_assert (items.find (id) == items.end ());
	try
	{
		auto wallet_l = std::make_unique<wallet_data> (transaction, *this, id, json);
		auto handle = wallet_l->handle;
		transaction.commit ();
		items[id] = std::move (wallet_l);
		return handle;
	}
	catch (std::exception const & ex)
	{
		logger.error (nano::log::type::wallet, "Failed to create wallet {} from JSON: {}", id, ex.what ());
	}
	return nullptr;
}

void wallets::search_receivable_all ()
{
	receivable_tracker.search_all ();
}

bool wallets::destroy (nano::wallet_id const & id)
{
	auto transaction = tx_begin_write ();
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto existing (items.find (id));
	if (existing == items.end ())
	{
		return false;
	}
	auto wallet_l = std::move (existing->second);
	items.erase (existing);
	wallet_l->store.destroy (transaction);
	return true;
}

void wallets::reload ()
{
	auto transaction = tx_begin_write ();
	nano::lock_guard<nano::mutex> lock{ mutex };
	std::unordered_set<nano::uint256_union> stored_items;
	for (auto it = backend.index_begin (transaction), end = backend.index_end (transaction); it != end; ++it)
	{
		// The wallet index range may also include entries for non-wallet sub-tables (e.g. `send_action_ids` on LMDB);
		// skip anything that doesn't parse as a 64-char hex wallet id.
		auto id = try_parse_wallet_id (bytes_to_string (it->first));
		if (!id)
		{
			continue;
		}
		// New wallet
		if (items.find (*id) == items.end ())
		{
			try
			{
				items[*id] = std::make_unique<wallet_data> (transaction, *this, *id);
			}
			catch (std::exception const & ex)
			{
				logger.error (nano::log::type::wallet, "Failed to open wallet {}: {}", *id, ex.what ());
			}
		}
		// List of wallets on disk
		stored_items.insert (*id);
	}
	// Delete non existing wallets from memory
	std::vector<nano::wallet_id> deleted_items;
	for (auto const & i : items)
	{
		if (stored_items.find (i.first) == stored_items.end ())
		{
			deleted_items.push_back (i.first);
		}
	}
	for (auto & i : deleted_items)
	{
		debug_assert (items.find (i) != items.end ());
		items.erase (i);
	}
}

bool wallets::exists (nano::account const & account)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto transaction = tx_begin_read ();
	return std::any_of (items.begin (), items.end (), [&] (auto const & item) {
		return item.second->store.exists (transaction, account);
	});
}

bool wallets::exists_any (nano::account const & account1, nano::account const & account2)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto transaction = tx_begin_read ();
	return std::any_of (items.begin (), items.end (), [&] (auto const & item) {
		return item.second->store.exists (transaction, account1) || item.second->store.exists (transaction, account2);
	});
}

nano::store::write_transaction wallets::tx_begin_write ()
{
	return backend.tx_begin_write ();
}

nano::store::read_transaction wallets::tx_begin_read () const
{
	return backend.tx_begin_read ();
}

void wallets::clear_send_ids ()
{
	auto transaction = tx_begin_write ();
	backend.send_action_ids_clear (transaction);
}

wallet_representatives wallets::reps () const
{
	return rep_tracker.reps ();
}

auto wallets::signer () -> signer_t
{
	return rep_tracker.signer ();
}

void wallets::refresh_reps ()
{
	rep_tracker.refresh ();
}

void wallets::foreach_representative (std::function<void (nano::public_key const & pub, nano::raw_key const & prv)> const & action)
{
	rep_tracker.foreach_representative (action);
}

void wallets::receive_confirmed (nano::block_hash const & hash, nano::account const & destination)
{
	receivable_tracker.receive_confirmed (hash, destination);
}

std::vector<std::pair<nano::wallet_id, nano::account>> wallets::holders (nano::account const & account) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto transaction = tx_begin_read ();
	std::vector<std::pair<nano::wallet_id, nano::account>> result;
	for (auto const & [id, wallet_l] : items)
	{
		if (wallet_l->store.exists (transaction, account))
		{
			result.emplace_back (id, wallet_l->store.representative (transaction));
		}
	}
	return result;
}

std::unordered_map<nano::wallet_id, std::shared_ptr<wallet>> wallets::all_wallets ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	std::unordered_map<nano::wallet_id, std::shared_ptr<wallet>> result;
	result.reserve (items.size ());
	for (auto const & [id, wallet_l] : items)
	{
		result.emplace (id, wallet_l->handle);
	}
	return result;
}

std::vector<nano::wallet_id> wallets::wallet_ids () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	std::vector<nano::wallet_id> result;
	result.reserve (items.size ());
	for (auto const & [id, wallet] : items)
	{
		result.push_back (id);
	}
	return result;
}

std::size_t wallets::wallet_count () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return items.size ();
}

/*
 * wallets — id-keyed operations
 */

wallet_data * wallets::find_wallet (nano::wallet_id const & id) const
{
	auto existing = items.find (id);
	return existing != items.end () ? existing->second.get () : nullptr;
}

void wallets::enter_initial_password (nano::wallet_id const & id)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return;
	}
	nano::raw_key password_l;
	wallet_l->store.password.value (password_l);
	if (password_l.is_zero ())
	{
		auto transaction = tx_begin_read ();
		enter_password_impl (*wallet_l, transaction, wallet_store::default_password);
	}
}

bool wallets::enter_password (nano::wallet_id const & id, std::string const & password)
{
	auto transaction = tx_begin_write ();
	nano::unique_lock<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return true;
	}
	auto result = enter_password_impl (*wallet_l, transaction, password);
	lock.unlock ();
	transaction.commit ();
	// Refresh even on failure: a failed attempt overwrites the password and locks the wallet, so cached rep keys must not outlive it
	rep_tracker.refresh ();
	return result;
}

bool wallets::enter_password_impl (wallet_data & wallet_l, nano::store::transaction const & transaction, std::string const & password)
{
	auto result (wallet_l.store.attempt_password (transaction, password));
	if (!result)
	{
		logger.info (nano::log::type::wallet, "Wallet unlocked");
		action_runner.queue (wallets_actions::high_priority, wallet_l.id, [] (wallet & wallet_l) {
			wallet_l.search_receivable ();
		});
	}
	else
	{
		logger.warn (nano::log::type::wallet, "Invalid password, wallet locked");
	}
	wallet_l.lock_observer (result, password.empty ());
	return result;
}

bool wallets::rekey (nano::wallet_id const & id, std::string const & password)
{
	auto transaction = tx_begin_write ();
	nano::unique_lock<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return true;
	}
	auto result = wallet_l->store.rekey (transaction, password);
	lock.unlock ();
	transaction.commit ();
	if (!result)
	{
		rep_tracker.refresh ();
	}
	return result;
}

bool wallets::is_locked (nano::wallet_id const & id) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return true;
	}
	auto transaction = tx_begin_read ();
	return !wallet_l->store.valid_password (transaction);
}

void wallets::lock (nano::wallet_id const & id)
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		auto wallet_l = find_wallet (id);
		if (!wallet_l)
		{
			return;
		}
		logger.info (nano::log::type::wallet, "Wallet locked");
		wallet_l->store.password_clear ();
	}
	rep_tracker.refresh ();
}

void wallets::set_lock_observer (nano::wallet_id const & id, std::function<void (bool, bool)> observer)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return;
	}
	wallet_l->lock_observer = std::move (observer);
}

nano::public_key wallets::deterministic_insert_impl (wallet_data & wallet_l, nano::store::write_transaction const & transaction, nano::wallet::wallet_cipher const & cipher, bool generate_work)
{
	auto key = wallet_l.store.deterministic_insert (transaction, cipher);

	logger.info (nano::log::type::wallet, "Deterministically inserted new account: {}", key.to_account ());

	if (generate_work)
	{
		action_runner.work_ensure (wallet_l.id, key, key);
	}

	return key;
}

nano::public_key wallets::deterministic_insert_impl (wallet_data & wallet_l, nano::store::write_transaction const & transaction, nano::wallet::wallet_cipher const & cipher, uint32_t index, bool generate_work)
{
	auto key = wallet_l.store.deterministic_insert (transaction, cipher, index);

	logger.info (nano::log::type::wallet, "Deterministically inserted new account: {} with index: {}", key.to_account (), index);

	if (generate_work)
	{
		action_runner.work_ensure (wallet_l.id, key, key);
	}

	return key;
}

nano::result<nano::public_key> wallets::deterministic_insert (nano::wallet_id const & id, uint32_t index, bool generate_work)
{
	auto transaction = tx_begin_write ();
	nano::unique_lock<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return nano::error (nano::error_common::wallet_not_found);
	}

	auto cipher = wallet_l->store.unlock (transaction);
	if (!cipher)
	{
		return nano::error (nano::error_common::wallet_locked);
	}

	auto result = deterministic_insert_impl (*wallet_l, transaction, cipher.value (), index, generate_work);
	lock.unlock ();
	transaction.commit ();
	rep_tracker.refresh ();
	return result;
}

nano::result<nano::public_key> wallets::deterministic_insert (nano::wallet_id const & id, bool generate_work)
{
	auto transaction = tx_begin_write ();
	nano::unique_lock<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return nano::error (nano::error_common::wallet_not_found);
	}

	auto cipher = wallet_l->store.unlock (transaction);
	if (!cipher)
	{
		return nano::error (nano::error_common::wallet_locked);
	}

	auto result = deterministic_insert_impl (*wallet_l, transaction, cipher.value (), generate_work);
	lock.unlock ();
	transaction.commit ();
	rep_tracker.refresh ();
	return result;
}

nano::result<nano::public_key> wallets::insert_adhoc (nano::wallet_id const & id, nano::raw_key const & prv, bool generate_work)
{
	auto transaction = tx_begin_write ();
	nano::unique_lock<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return nano::error (nano::error_common::wallet_not_found);
	}

	auto cipher = wallet_l->store.unlock (transaction);
	if (!cipher)
	{
		return nano::error (nano::error_common::wallet_locked);
	}

	auto key = wallet_l->store.insert_adhoc (transaction, cipher.value (), prv);

	logger.info (nano::log::type::wallet, "Ad-hoc inserted new account: {}", key.to_account ());

	if (generate_work)
	{
		auto ledger_txn = ledger.tx_begin_read ();
		action_runner.work_ensure (id, key, ledger.latest_root (ledger_txn, key));
	}

	// Makes sure that the representatives container will be in sync with any added keys
	lock.unlock ();
	transaction.commit ();
	rep_tracker.refresh ();

	return key;
}

bool wallets::insert_watch (nano::wallet_id const & id, nano::public_key const & pub)
{
	auto transaction = tx_begin_write ();
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return true;
	}
	return wallet_l->store.insert_watch (transaction, pub);
}

void wallets::remove_account (nano::wallet_id const & id, nano::account const & account)
{
	auto transaction = tx_begin_write ();
	nano::unique_lock<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return;
	}
	wallet_l->store.erase (transaction, account);
	lock.unlock ();
	transaction.commit ();
	rep_tracker.refresh ();
}

std::vector<nano::account> wallets::accounts (nano::wallet_id const & id) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return {};
	}
	auto transaction = tx_begin_read ();
	return wallet_l->store.accounts (transaction);
}

bool wallets::exists (nano::wallet_id const & id, nano::account const & account) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return false;
	}
	auto transaction = tx_begin_read ();
	return wallet_l->store.exists (transaction, account);
}

nano::result<bool> wallets::move_accounts (nano::wallet_id const & target, nano::wallet_id const & source, std::vector<nano::public_key> const & accounts)
{
	auto transaction = tx_begin_write ();
	nano::unique_lock<nano::mutex> lock{ mutex };
	auto target_l = find_wallet (target);
	auto source_l = find_wallet (source);
	if (!target_l || !source_l)
	{
		return nano::error (nano::error_common::wallet_not_found);
	}
	nano::result<bool> result{ true };
	result = target_l->store.move (transaction, source_l->store, accounts);
	lock.unlock ();
	transaction.commit ();
	rep_tracker.refresh ();
	return result;
}

key_type wallets::key_type (nano::wallet_id const & id, nano::account const & account) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return key_type::not_a_type;
	}
	auto transaction = tx_begin_read ();
	auto value = wallet_l->store.entry_get_raw (transaction, account);
	return wallet_l->store.key_type (value);
}

nano::result<nano::raw_key> wallets::get_seed (nano::wallet_id const & id) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return nano::error (nano::error_common::wallet_not_found);
	}
	auto transaction = tx_begin_read ();
	auto cipher = wallet_l->store.unlock (transaction);
	if (!cipher)
	{
		return nano::error (nano::error_common::wallet_locked);
	}
	return wallet_l->store.seed (transaction, cipher.value ());
}

nano::result<nano::public_key> wallets::change_seed (nano::wallet_id const & id, nano::raw_key const & seed, uint32_t count)
{
	auto transaction = tx_begin_write ();
	nano::unique_lock<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return nano::error (nano::error_common::wallet_not_found);
	}
	auto cipher = wallet_l->store.unlock (transaction);
	if (!cipher)
	{
		return nano::error (nano::error_common::wallet_locked);
	}
	auto result = change_seed_impl (*wallet_l, transaction, cipher.value (), seed, count);
	lock.unlock ();
	transaction.commit ();
	rep_tracker.refresh ();
	return result;
}

void wallets::deterministic_restore (nano::wallet_id const & id)
{
	auto transaction = tx_begin_write ();
	nano::unique_lock<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return;
	}
	auto cipher = wallet_l->store.unlock (transaction);
	if (!cipher)
	{
		return;
	}
	// Scan the ledger for used accounts beyond those already inserted
	if (auto last = deterministic_check_impl (*wallet_l, transaction, cipher.value (), wallet_l->store.deterministic_index_get (transaction)))
	{
		deterministic_insert_up_to_impl (*wallet_l, transaction, cipher.value (), *last);
	}
	lock.unlock ();
	transaction.commit ();
	rep_tracker.refresh ();
}

std::optional<uint32_t> wallets::deterministic_check (nano::wallet_id const & id, uint32_t index) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return std::nullopt;
	}
	auto transaction = tx_begin_read ();
	auto cipher = wallet_l->store.unlock (transaction);
	if (!cipher)
	{
		return std::nullopt;
	}
	return deterministic_check_impl (*wallet_l, transaction, cipher.value (), index);
}

std::optional<uint32_t> wallets::deterministic_check_impl (wallet_data const & wallet_l, nano::store::transaction const & transaction, nano::wallet::wallet_cipher const & cipher, uint32_t index) const
{
	auto ledger_txn = ledger.tx_begin_read ();
	std::optional<uint32_t> result;
	for (uint32_t i (index), n (index + deterministic_check_gap); i < n; ++i)
	{
		auto prv = wallet_l.store.deterministic_key (transaction, cipher, i);
		nano::keypair pair (prv.to_string ());
		// Check if account received at least 1 block
		auto latest (ledger.any.account_head (ledger_txn, pair.pub));
		if (!latest.is_zero ())
		{
			result = i;
			// Scan a full gap beyond the hit, plus i/gap extra for large wallets
			n = i + 1 + deterministic_check_gap + (i / deterministic_check_gap);
		}
		else
		{
			// Check if there are pending blocks for account
			auto current = ledger.any.receivable_upper_bound (ledger_txn, pair.pub, 0);
			if (current != ledger.any.receivable_end ())
			{
				result = i;
				n = i + 1 + deterministic_check_gap + (i / deterministic_check_gap);
			}
		}
	}
	return result;
}

std::optional<nano::public_key> wallets::deterministic_insert_up_to_impl (wallet_data & wallet_l, nano::store::write_transaction const & transaction, nano::wallet::wallet_cipher const & cipher, uint32_t last)
{
	std::optional<nano::public_key> account;
	// The stored index is re-read each round because an insert skips over indexes whose accounts already exist
	for (uint64_t index = wallet_l.store.deterministic_index_get (transaction); index <= last;)
	{
		// Disable work generation to prevent weak CPU nodes stuck
		account = deterministic_insert_impl (wallet_l, transaction, cipher, false);
		uint64_t next = wallet_l.store.deterministic_index_get (transaction);
		// The index wraps at the end of its range, stop instead of inserting forever
		if (next <= index)
		{
			break;
		}
		index = next;
	}
	return account;
}

nano::public_key wallets::change_seed_impl (wallet_data & wallet_l, nano::store::write_transaction const & transaction, nano::wallet::wallet_cipher const & cipher, nano::raw_key const & seed, uint32_t count)
{
	logger.info (nano::log::type::wallet, "Changing wallet seed");

	wallet_l.store.seed_set (transaction, cipher, seed);
	// The wallet contains at least the first seed account
	auto account = deterministic_insert_impl (wallet_l, transaction, cipher);
	// An explicit count requests accounts 0..count inclusive, otherwise the ledger scan finds the highest account in use
	std::optional<uint32_t> last;
	if (count == 0)
	{
		last = deterministic_check_impl (wallet_l, transaction, cipher, wallet_l.store.deterministic_index_get (transaction));
		if (last)
		{
			logger.info (nano::log::type::wallet, "Auto-detected used accounts up to index {} to restore from seed", *last);
		}
	}
	else
	{
		last = count;
	}
	if (last)
	{
		if (auto inserted = deterministic_insert_up_to_impl (wallet_l, transaction, cipher, *last))
		{
			account = *inserted;
		}
	}

	logger.info (nano::log::type::wallet, "Completed changing wallet seed and generating accounts");

	return account;
}

uint32_t wallets::get_deterministic_index (nano::wallet_id const & id) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return 0;
	}
	auto transaction = tx_begin_read ();
	return wallet_l->store.deterministic_index_get (transaction);
}

void wallets::set_representative (nano::wallet_id const & id, nano::account const & rep)
{
	auto transaction = tx_begin_write ();
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return;
	}
	wallet_l->store.representative_set (transaction, rep);
}

nano::account wallets::get_representative (nano::wallet_id const & id) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return 0;
	}
	auto transaction = tx_begin_read ();
	return wallet_l->store.representative (transaction);
}

std::unordered_set<nano::account> wallets::reps (nano::wallet_id const & id) const
{
	return rep_tracker.reps (id);
}

nano::result<nano::raw_key> wallets::fetch_prv (nano::wallet_id const & id, nano::account const & account) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return nano::error (nano::error_common::wallet_not_found);
	}
	auto transaction = tx_begin_read ();
	return wallet_l->store.fetch (transaction, account);
}

std::shared_ptr<nano::block> wallets::receive_action (nano::wallet_id const & id, nano::block_hash const & send_hash, nano::account const & representative, nano::uint128_union const & amount, nano::account const & account, uint64_t work, bool generate_work)
{
	return action_runner.receive_action (id, send_hash, representative, amount, account, work, generate_work);
}

prepared_block wallets::prepare_receive (nano::wallet_id const & id, nano::block_hash const & send_hash, nano::account const & representative, nano::uint128_union const & amount, nano::account const & account, uint64_t work)
{
	std::shared_ptr<nano::block> block;
	nano::block_details details;
	details.is_receive = true;
	if (config.receive_minimum.number () <= amount.number ())
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		auto wallet_l = find_wallet (id);
		if (!wallet_l)
		{
			logger.warn (nano::log::type::wallet, "Not receiving block: {}, wallet no longer exists", send_hash);
			return {};
		}
		auto ledger_txn = ledger.tx_begin_read ();
		auto transaction = tx_begin_read ();
		if (ledger.any.block_exists_or_pruned (ledger_txn, send_hash))
		{
			auto pending_info = ledger.any.pending_get (ledger_txn, nano::pending_key (account, send_hash));
			if (pending_info)
			{
				auto prv_result = wallet_l->store.fetch (transaction, account);
				if (prv_result)
				{
					logger.info (nano::log::type::wallet, "Receiving block: {} from account: {}, amount: {} raw",
					send_hash,
					account,
					nano::log::as_raw_nano (pending_info->amount));

					if (work == 0)
					{
						work = wallet_l->store.work_get (transaction, account).value_or (0);
					}
					auto info = ledger.any.account_get (ledger_txn, account);
					if (info)
					{
						block = std::make_shared<nano::state_block> (account, info->head, info->representative, info->balance.number () + pending_info->amount.number (), send_hash, prv_result.value (), account, work);
						details.epoch = std::max (info->epoch (), pending_info->epoch);
					}
					else
					{
						block = std::make_shared<nano::state_block> (account, 0, representative, pending_info->amount, reinterpret_cast<nano::link const &> (send_hash), prv_result.value (), account, work);
						details.epoch = pending_info->epoch;
					}
				}
				else
				{
					logger.warn (nano::log::type::wallet, "Unable to receive, wallet locked, block: {} to account: {}",
					send_hash,
					account);
				}
			}
			else
			{
				// Ledger doesn't have this marked as available to receive anymore
				logger.warn (nano::log::type::wallet, "Not receiving block: {}, block already received", send_hash);
			}
		}
		else
		{
			// Ledger doesn't have this block anymore.
			logger.warn (nano::log::type::wallet, "Not receiving block: {}, block no longer exists or pruned", send_hash);
		}
	}
	else
	{
		// Someone sent us something below the threshold of receiving
		logger.warn (nano::log::type::wallet, "Not receiving block: {} due to minimum receive threshold", send_hash);
	}
	return { block, details };
}

std::shared_ptr<nano::block> wallets::change_action (nano::wallet_id const & id, nano::account const & source, nano::account const & representative, uint64_t work, bool generate_work)
{
	return action_runner.change_action (id, source, representative, work, generate_work);
}

prepared_block wallets::prepare_change (nano::wallet_id const & id, nano::account const & source, nano::account const & representative, uint64_t work)
{
	std::shared_ptr<nano::block> block;
	nano::block_details details;
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		auto wallet_l = find_wallet (id);
		if (!wallet_l)
		{
			logger.warn (nano::log::type::wallet, "Changing representative for account: {} failed, wallet no longer exists", source);
			return {};
		}
		auto transaction = tx_begin_read ();
		auto ledger_txn = ledger.tx_begin_read ();
		if (wallet_l->store.valid_password (transaction))
		{
			auto existing (wallet_l->store.find (transaction, source));
			if (existing != wallet_l->store.end (transaction) && !ledger.any.account_head (ledger_txn, source).is_zero ())
			{
				logger.info (nano::log::type::wallet, "Changing representative for account: {} to: {}",
				source,
				representative);

				auto info = ledger.any.account_get (ledger_txn, source);
				release_assert (info, "could not find account info for account in wallet change_action", source.to_account ());
				auto prv_result = wallet_l->store.fetch (transaction, source);
				release_assert (prv_result, "failed to fetch private key for account in wallet change_action", source.to_account ());
				if (work == 0)
				{
					work = wallet_l->store.work_get (transaction, source).value_or (0);
				}
				block = std::make_shared<nano::state_block> (source, info->head, representative, info->balance, 0, prv_result.value (), source, work);
				details.epoch = info->epoch ();
			}
			else
			{
				logger.warn (nano::log::type::wallet, "Changing representative for account: {} failed, wallet locked or account not found", source);
			}
		}
		else
		{
			logger.warn (nano::log::type::wallet, "Changing representative for account: {} failed, wallet locked", source);
		}
	}
	return { block, details };
}

std::shared_ptr<nano::block> wallets::send_action (nano::wallet_id const & id, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, uint64_t work, bool generate_work, std::optional<std::string> send_id)
{
	return action_runner.send_action (id, source, destination, amount, work, generate_work, send_id);
}

prepared_send wallets::prepare_send (nano::wallet_id const & id, nano::account const & source, nano::account const & account, nano::uint128_t const & amount, uint64_t work, std::optional<std::string> send_id)
{
	auto prepare = [this, &source, &amount, &work, &account, &send_id] (auto const & transaction, wallet_data & wallet_l) {
		auto ledger_txn = ledger.tx_begin_read ();
		auto error (false);
		auto cached_block (false);
		std::shared_ptr<nano::block> block;
		nano::block_details details;
		details.is_send = true;
		if (send_id)
		{
			auto existing_value = backend.send_action_id_get (transaction, *send_id);
			if (existing_value)
			{
				auto existing_hash = static_cast<nano::block_hash> (*existing_value);
				block = ledger.any.block_get (ledger_txn, existing_hash);
				if (block != nullptr)
				{
					logger.warn (nano::log::type::wallet, "Block already exists for send action with id: {}, existing hash: {}",
					send_id.value (),
					existing_hash);

					cached_block = true;
				}
				else
				{
					logger.warn (nano::log::type::wallet, "Block was not found in ledger for send action with id: {}, hash: {}",
					send_id.value (),
					existing_hash);
				}
			}
		}
		if (!error && block == nullptr)
		{
			if (wallet_l.store.valid_password (transaction))
			{
				auto existing (wallet_l.store.find (transaction, source));
				if (existing != wallet_l.store.end (transaction))
				{
					auto balance (ledger.any.account_balance (ledger_txn, source));
					if (balance && balance.value ().number () >= amount)
					{
						logger.info (nano::log::type::wallet, "Sending from account: {} to: {}, amount: {} raw",
						source,
						account,
						nano::log::as_raw_nano (amount));

						auto info = ledger.any.account_get (ledger_txn, source);
						release_assert (info, "could not find account info for account in wallet send_action", source.to_account ());
						auto prv_result = wallet_l.store.fetch (transaction, source);
						release_assert (prv_result, "failed to fetch private key for account in wallet send_action", source.to_account ());
						if (work == 0)
						{
							work = wallet_l.store.work_get (transaction, source).value_or (0);
						}
						block = std::make_shared<nano::state_block> (source, info->head, info->representative, balance.value ().number () - amount, account, prv_result.value (), source, work);
						details.epoch = info->epoch ();
						if (send_id && block != nullptr)
						{
							// `send_id` being set implies the caller passed a write transaction (see below).
							// `if constexpr` keeps the put out of the read-txn instantiation of this lambda.
							if constexpr (std::is_same_v<std::decay_t<decltype (transaction)>, nano::store::write_transaction>)
							{
								if (!backend.send_action_id_put (transaction, *send_id, block->hash ()))
								{
									block = nullptr;
									error = true;
								}
							}
							else
							{
								release_assert (false, "send_action with id requires a write transaction");
							}
						}
					}
					else
					{
						if (balance)
						{
							logger.warn (nano::log::type::wallet, "Insufficient balance for send from: {}, required: {} raw, available: {} raw",
							source,
							nano::log::as_raw_nano (amount),
							nano::log::as_raw_nano (balance.value ()));
						}
						else
						{
							logger.warn (nano::log::type::wallet, "Insufficient balance for send from: {}, required: {} raw, available: unknown",
							source,
							nano::log::as_raw_nano (amount));
						}
					}
				}
			}
		}
		return std::make_tuple (block, error, cached_block, details);
	};

	std::tuple<std::shared_ptr<nano::block>, bool, bool, nano::block_details> result;
	if (send_id)
	{
		// A send id requires a write transaction to atomically record the id -> block mapping
		auto transaction = tx_begin_write ();
		nano::lock_guard<nano::mutex> lock{ mutex };
		auto wallet_l = find_wallet (id);
		if (!wallet_l)
		{
			logger.warn (nano::log::type::wallet, "Sending from account: {} failed, wallet no longer exists", source);
			return {};
		}
		result = prepare (transaction, *wallet_l);
	}
	else
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		auto wallet_l = find_wallet (id);
		if (!wallet_l)
		{
			logger.warn (nano::log::type::wallet, "Sending from account: {} failed, wallet no longer exists", source);
			return {};
		}
		result = prepare (tx_begin_read (), *wallet_l);
	}

	std::shared_ptr<nano::block> block;
	bool error;
	bool cached_block;
	nano::block_details details;
	std::tie (block, error, cached_block, details) = result;

	return { block, details, error, cached_block };
}

bool wallets::change_sync (nano::wallet_id const & id, nano::account const & source, nano::account const & representative)
{
	return action_runner.change_sync (id, source, representative);
}

void wallets::change_async (nano::wallet_id const & id, nano::account const & source, nano::account const & representative, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work, bool generate_work)
{
	action_runner.change_async (id, source, representative, action, work, generate_work);
}

bool wallets::receive_sync (nano::wallet_id const & id, std::shared_ptr<nano::block> const & block, nano::account const & representative, nano::uint128_t const & amount)
{
	return action_runner.receive_sync (id, block, representative, amount);
}

void wallets::receive_async (nano::wallet_id const & id, nano::block_hash const & hash, nano::account const & representative, nano::uint128_t const & amount, nano::account const & account, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work, bool generate_work)
{
	action_runner.receive_async (id, hash, representative, amount, account, action, work, generate_work);
}

nano::block_hash wallets::send_sync (nano::wallet_id const & id, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount)
{
	return action_runner.send_sync (id, source, destination, amount);
}

void wallets::send_async (nano::wallet_id const & id, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work, bool generate_work, std::optional<std::string> send_id)
{
	action_runner.send_async (id, source, destination, amount, action, work, generate_work, send_id);
}

// Update work for account if latest root is root
void wallets::work_update_impl (wallet_data & wallet_l, nano::store::write_transaction const & transaction, nano::account const & account, nano::root const & root, uint64_t work)
{
	debug_assert (!network_params.work.validate_entry (nano::work_version::work_1, root, work));
	debug_assert (wallet_l.store.exists (transaction, account));
	auto ledger_txn = ledger.tx_begin_read ();
	auto latest (ledger.latest_root (ledger_txn, account));
	if (latest == root)
	{
		wallet_l.store.work_put (transaction, account, work);
	}
	else
	{
		logger.warn (nano::log::type::wallet, "Cached work no longer valid, discarding");
	}
}

void wallets::work_cache_blocking (nano::wallet_id const & id, nano::account const & account, nano::root const & root)
{
	action_runner.work_cache_blocking (id, account, root);
}

void wallets::update_work (nano::wallet_id const & id, nano::account const & account, nano::root const & root, uint64_t work)
{
	auto transaction = tx_begin_write ();
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (wallet_l && wallet_l->store.exists (transaction, account))
	{
		work_update_impl (*wallet_l, transaction, account, root, work);
	}
}

void wallets::work_ensure (nano::wallet_id const & id, nano::account const & account, nano::root const & root)
{
	action_runner.work_ensure (id, account, root);
}

void wallets::set_observer (std::function<void (bool)> observer)
{
	action_runner.set_observer (std::move (observer));
}

nano::result<uint64_t> wallets::get_work (nano::wallet_id const & id, nano::public_key const & pub) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return nano::error (nano::error_common::account_not_found_wallet);
	}
	auto transaction = tx_begin_read ();
	auto result = wallet_l->store.work_get (transaction, pub);
	if (result)
	{
		return *result;
	}
	return nano::error (nano::error_common::account_not_found_wallet);
}

void wallets::set_work (nano::wallet_id const & id, nano::public_key const & pub, uint64_t work)
{
	auto transaction = tx_begin_write ();
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return;
	}
	wallet_l->store.work_put (transaction, pub, work);
}

bool wallets::search_receivable (nano::wallet_id const & id)
{
	return receivable_tracker.search (id);
}

nano::result<wallet_scan_info> wallets::scan_info (nano::wallet_id const & id) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return nano::error (nano::error_common::wallet_not_found);
	}
	auto transaction = tx_begin_read ();
	if (!wallet_l->store.valid_password (transaction))
	{
		return nano::error (nano::error_common::wallet_locked);
	}
	wallet_scan_info result;
	result.representative = wallet_l->store.representative (transaction);
	for (auto i (wallet_l->store.begin (transaction)), n (wallet_l->store.end (transaction)); i != n; ++i)
	{
		// Watch-only accounts have no key to receive with
		if (!nano::wallet::wallet_value (i->second).key.is_zero ())
		{
			result.accounts.push_back (i->first);
		}
	}
	return result;
}

bool wallets::import (nano::wallet_id const & id, std::string const & json, std::string const & password)
{
	auto transaction = tx_begin_write ();
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return true;
	}
	bool error (true);
	nano::uint256_union temp_id;
	random_pool::generate_block (temp_id.bytes.data (), temp_id.bytes.size ());
	try
	{
		auto temp = std::make_unique<wallet_store> (kdf, transaction, backend, 1, temp_id.to_string (), json);
		if (!temp->attempt_password (transaction, password))
		{
			auto result = wallet_l->store.import (transaction, *temp);
			error = !result || result.value ();
		}
		temp->destroy (transaction);
	}
	catch (std::exception const & ex)
	{
		logger.error (nano::log::type::wallet, "Failed to import wallet: {}", ex.what ());
	}
	return error;
}

void wallets::serialize_json (nano::wallet_id const & id, std::string & json) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return;
	}
	auto transaction = tx_begin_read ();
	wallet_l->store.serialize_json (transaction, json);
}

void wallets::write_backup (nano::wallet_id const & id, std::filesystem::path const & path) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	if (!wallet_l)
	{
		return;
	}
	auto transaction = tx_begin_read ();
	wallet_l->store.write_backup (transaction, path);
}

nano::fan & wallets::password_fan (nano::wallet_id const & id)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto wallet_l = find_wallet (id);
	release_assert (wallet_l, "password fan requested for missing wallet");
	return wallet_l->store.password;
}

nano::container_info wallets::container_info () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	nano::container_info info;
	info.put ("items", items.size ());
	info.add ("actions", action_runner.container_info ());
	info.add ("reps", rep_tracker.container_info ());
	return info;
}
}
