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
	core{ *this, backend, ledger, config, network_params, logger },
	rep_tracker{ *this, node, ledger, config, network_params, stats, logger },
	receivable_tracker{ *this, node, ledger, config, network_params, stats, logger },
	action_runner{ *this, node, config, network_params, network, logger }
{
	// Enter initial passwords, wallets that unlock get a receivable search queued
	for (auto const & id : core.wallet_ids ())
	{
		enter_initial_password (id);
	}
}

std::shared_ptr<wallet> wallets::open (nano::wallet_id const & id)
{
	return core.open (id);
}

std::shared_ptr<wallet> wallets::create (nano::wallet_id const & id)
{
	return core.create (id);
}

std::shared_ptr<wallet> wallets::create_from_json (nano::wallet_id const & id, std::string const & json)
{
	return core.create_from_json (id, json);
}

bool wallets::destroy (nano::wallet_id const & id)
{
	return core.destroy (id);
}

void wallets::reload ()
{
	core.reload ();
}

void wallets::clear_send_ids ()
{
	core.clear_send_ids ();
}

std::unordered_map<nano::wallet_id, std::shared_ptr<wallet>> wallets::all_wallets ()
{
	return core.all_wallets ();
}

std::vector<nano::wallet_id> wallets::wallet_ids () const
{
	return core.wallet_ids ();
}

std::size_t wallets::wallet_count () const
{
	return core.wallet_count ();
}

bool wallets::exists (nano::account const & account)
{
	return core.exists (account);
}

bool wallets::exists_any (nano::account const & account1, nano::account const & account2)
{
	return core.exists_any (account1, account2);
}

void wallets::enter_initial_password (nano::wallet_id const & id)
{
	if (core.enter_initial_password (id))
	{
		// Newly unlocked wallets may have receivables waiting
		action_runner.queue (wallets_actions::high_priority, id, [] (wallet & wallet_l) {
			wallet_l.search_receivable ();
		});
	}
}

bool wallets::enter_password (nano::wallet_id const & id, std::string const & password)
{
	auto result = core.enter_password (id, password);
	if (!result)
	{
		// Newly unlocked wallets may have receivables waiting
		action_runner.queue (wallets_actions::high_priority, id, [] (wallet & wallet_l) {
			wallet_l.search_receivable ();
		});
	}
	// Refresh even on failure: a failed attempt overwrites the password and locks the wallet, so cached rep keys must not outlive it
	rep_tracker.refresh ();
	return result;
}

bool wallets::rekey (nano::wallet_id const & id, std::string const & password)
{
	auto result = core.rekey (id, password);
	if (!result)
	{
		rep_tracker.refresh ();
	}
	return result;
}

bool wallets::is_locked (nano::wallet_id const & id) const
{
	return core.is_locked (id);
}

void wallets::lock (nano::wallet_id const & id)
{
	if (core.lock (id))
	{
		rep_tracker.refresh ();
	}
}

void wallets::set_lock_observer (nano::wallet_id const & id, std::function<void (bool, bool)> observer)
{
	core.set_lock_observer (id, std::move (observer));
}

nano::result<nano::public_key> wallets::insert_adhoc (nano::wallet_id const & id, nano::raw_key const & prv, bool generate_work)
{
	auto result = core.insert_adhoc (id, prv);
	if (result)
	{
		if (generate_work)
		{
			auto ledger_txn = ledger.tx_begin_read ();
			action_runner.work_ensure (id, result.value (), ledger.latest_root (ledger_txn, result.value ()));
		}
		// Makes sure that the representatives container will be in sync with any added keys
		rep_tracker.refresh ();
	}
	return result;
}

nano::result<nano::public_key> wallets::deterministic_insert (nano::wallet_id const & id, uint32_t index, bool generate_work)
{
	auto result = core.deterministic_insert (id, index);
	if (result)
	{
		if (generate_work)
		{
			action_runner.work_ensure (id, result.value (), result.value ());
		}
		rep_tracker.refresh ();
	}
	return result;
}

nano::result<nano::public_key> wallets::deterministic_insert (nano::wallet_id const & id, bool generate_work)
{
	auto result = core.deterministic_insert (id);
	if (result)
	{
		if (generate_work)
		{
			action_runner.work_ensure (id, result.value (), result.value ());
		}
		rep_tracker.refresh ();
	}
	return result;
}

bool wallets::insert_watch (nano::wallet_id const & id, nano::public_key const & pub)
{
	return core.insert_watch (id, pub);
}

void wallets::remove_account (nano::wallet_id const & id, nano::account const & account)
{
	core.remove_account (id, account);
	rep_tracker.refresh ();
}

std::vector<nano::account> wallets::accounts (nano::wallet_id const & id) const
{
	return core.accounts (id);
}

bool wallets::exists (nano::wallet_id const & id, nano::account const & account) const
{
	return core.exists (id, account);
}

nano::result<bool> wallets::move_accounts (nano::wallet_id const & target, nano::wallet_id const & source, std::vector<nano::public_key> const & accounts)
{
	auto result = core.move_accounts (target, source, accounts);
	rep_tracker.refresh ();
	return result;
}

key_type wallets::key_type (nano::wallet_id const & id, nano::account const & account) const
{
	return core.key_type (id, account);
}

nano::result<nano::raw_key> wallets::get_seed (nano::wallet_id const & id) const
{
	return core.get_seed (id);
}

nano::result<nano::public_key> wallets::change_seed (nano::wallet_id const & id, nano::raw_key const & seed, uint32_t count)
{
	auto result = core.change_seed (id, seed, count);
	if (result)
	{
		action_runner.work_ensure (id, result.value (), result.value ());
		rep_tracker.refresh ();
	}
	return result;
}

void wallets::deterministic_restore (nano::wallet_id const & id)
{
	core.deterministic_restore (id);
	rep_tracker.refresh ();
}

std::optional<uint32_t> wallets::deterministic_check (nano::wallet_id const & id, uint32_t index) const
{
	return core.deterministic_check (id, index);
}

uint32_t wallets::get_deterministic_index (nano::wallet_id const & id) const
{
	return core.get_deterministic_index (id);
}

void wallets::set_representative (nano::wallet_id const & id, nano::account const & rep)
{
	core.set_representative (id, rep);
}

nano::account wallets::get_representative (nano::wallet_id const & id) const
{
	return core.get_representative (id);
}

nano::result<nano::raw_key> wallets::fetch_prv (nano::wallet_id const & id, nano::account const & account) const
{
	return core.fetch_prv (id, account);
}

prepared_block wallets::prepare_receive (nano::wallet_id const & id, nano::block_hash const & send_hash, nano::account const & representative, nano::uint128_union const & amount, nano::account const & account, uint64_t work)
{
	return core.prepare_receive (id, send_hash, representative, amount, account, work);
}

prepared_block wallets::prepare_change (nano::wallet_id const & id, nano::account const & source, nano::account const & representative, uint64_t work)
{
	return core.prepare_change (id, source, representative, work);
}

prepared_send wallets::prepare_send (nano::wallet_id const & id, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, uint64_t work, std::optional<std::string> send_id)
{
	return core.prepare_send (id, source, destination, amount, work, send_id);
}

void wallets::update_work (nano::wallet_id const & id, nano::account const & account, nano::root const & root, uint64_t work)
{
	core.update_work (id, account, root, work);
}

nano::result<uint64_t> wallets::get_work (nano::wallet_id const & id, nano::public_key const & pub) const
{
	return core.get_work (id, pub);
}

void wallets::set_work (nano::wallet_id const & id, nano::public_key const & pub, uint64_t work)
{
	core.set_work (id, pub, work);
}

nano::result<wallet_scan_info> wallets::scan_info (nano::wallet_id const & id) const
{
	return core.scan_info (id);
}

std::vector<std::pair<nano::wallet_id, nano::account>> wallets::holders (nano::account const & account) const
{
	return core.holders (account);
}

bool wallets::import (nano::wallet_id const & id, std::string const & json, std::string const & password)
{
	return core.import (id, json, password);
}

void wallets::serialize_json (nano::wallet_id const & id, std::string & json) const
{
	core.serialize_json (id, json);
}

void wallets::write_backup (nano::wallet_id const & id, std::filesystem::path const & path) const
{
	core.write_backup (id, path);
}

nano::fan & wallets::password_fan (nano::wallet_id const & id)
{
	return core.password_fan (id);
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

void wallets::search_receivable_all ()
{
	receivable_tracker.search_all ();
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

/*
 * wallets — id-keyed operations
 */

std::unordered_set<nano::account> wallets::reps (nano::wallet_id const & id) const
{
	return rep_tracker.reps (id);
}

std::shared_ptr<nano::block> wallets::receive_action (nano::wallet_id const & id, nano::block_hash const & send_hash, nano::account const & representative, nano::uint128_union const & amount, nano::account const & account, uint64_t work, bool generate_work)
{
	return action_runner.receive_action (id, send_hash, representative, amount, account, work, generate_work);
}

std::shared_ptr<nano::block> wallets::change_action (nano::wallet_id const & id, nano::account const & source, nano::account const & representative, uint64_t work, bool generate_work)
{
	return action_runner.change_action (id, source, representative, work, generate_work);
}

std::shared_ptr<nano::block> wallets::send_action (nano::wallet_id const & id, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, uint64_t work, bool generate_work, std::optional<std::string> send_id)
{
	return action_runner.send_action (id, source, destination, amount, work, generate_work, send_id);
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

void wallets::work_cache_blocking (nano::wallet_id const & id, nano::account const & account, nano::root const & root)
{
	action_runner.work_cache_blocking (id, account, root);
}

void wallets::work_ensure (nano::wallet_id const & id, nano::account const & account, nano::root const & root)
{
	action_runner.work_ensure (id, account, root);
}

void wallets::set_observer (std::function<void (bool)> observer)
{
	action_runner.set_observer (std::move (observer));
}

bool wallets::search_receivable (nano::wallet_id const & id)
{
	return receivable_tracker.search (id);
}

nano::container_info wallets::container_info () const
{
	nano::container_info info;
	info.add ("core", core.container_info ());
	info.add ("actions", action_runner.container_info ());
	info.add ("reps", rep_tracker.container_info ());
	return info;
}
}
