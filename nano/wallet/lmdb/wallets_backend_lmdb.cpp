#include <nano/store/lmdb/common.hpp>
#include <nano/store/lmdb/db_val.hpp>
#include <nano/store/lmdb/iterator.hpp>
#include <nano/store/lmdb/utility.hpp>
#include <nano/wallet/lmdb/wallets_backend_lmdb.hpp>

#include <stdexcept>

namespace nano::wallet
{
wallets_backend_lmdb::wallets_backend_lmdb (std::filesystem::path const & path_a, nano::lmdb_config const & lmdb_config_a) :
	environment (path_a, nano::store::lmdb::env::options::make ().set_config (lmdb_config_a).override_config_sync (nano::lmdb_config::sync_strategy::always).override_config_map_size (1ULL * 1024 * 1024 * 1024))
{
	auto transaction = tx_begin_write ();
	auto status = mdb_dbi_open (environment.tx (transaction), nullptr, MDB_CREATE, &main_handle);
	status |= mdb_dbi_open (environment.tx (transaction), "send_action_ids", MDB_CREATE, &send_action_ids_handle);
	release_assert (nano::store::lmdb::success (status), nano::store::lmdb::error_string (status));
}

nano::store::read_transaction wallets_backend_lmdb::tx_begin_read () const
{
	return environment.tx_begin_read ();
}

nano::store::write_transaction wallets_backend_lmdb::tx_begin_write ()
{
	return environment.tx_begin_write ();
}

auto wallets_backend_lmdb::open_wallet (nano::store::write_transaction const & transaction_a, nano::wallet_id const & wallet_id_a) -> wallet_handle
{
	open_wallet_handle (transaction_a, wallet_id_a, true);
	return wallet_handle{ wallet_id_a };
}

void wallets_backend_lmdb::destroy_wallet (nano::store::write_transaction const & transaction_a, wallet_handle const & wallet_a)
{
	auto handle = table_handle (wallet_a);
	auto status = mdb_drop (environment.tx (transaction_a), handle, 1);
	release_assert (nano::store::lmdb::success (status), nano::store::lmdb::error_string (status));

	nano::lock_guard<nano::mutex> guard{ mutex };
	wallet_handles.erase (wallet_a.id);
}

std::vector<nano::wallet_id> wallets_backend_lmdb::wallet_ids (nano::store::transaction const & transaction_a) const
{
	std::vector<nano::wallet_id> result;
	// The LMDB main catalog stores all named tables. Wallet tables are named by the
	// hex encoding of wallet_id, so enumerate only that lexical key range here.
	std::string beginning{ nano::wallet_id{ 0 }.to_string () };
	nano::wallet_id end_id;
	end_id.bytes.fill (0xff);
	std::string end{ end_id.to_string () };
	nano::store::lmdb::db_val beginning_val{ beginning.size (), const_cast<char *> (beginning.c_str ()) };
	nano::store::lmdb::db_val end_val{ end.size (), const_cast<char *> (end.c_str ()) };
	auto mdb_beginning_val = nano::store::lmdb::to_mdb_val (beginning_val);
	auto mdb_end_val = nano::store::lmdb::to_mdb_val (end_val);
	nano::store::iterator i{ transaction_a, nano::store::lmdb::iterator::lower_bound (environment.tx (transaction_a), main_handle, mdb_beginning_val) };
	nano::store::iterator n{ transaction_a, nano::store::lmdb::iterator::lower_bound (environment.tx (transaction_a), main_handle, mdb_end_val) };

	for (; i != n; ++i)
	{
		nano::wallet_id id;
		std::string text (reinterpret_cast<char const *> (i->first.data ()), i->first.size ());
		auto error = id.decode_hex (text);
		release_assert (!error, "failed to decode wallet id from text: {}", text);
		result.push_back (id);
	}

	return result;
}

wallet_value wallets_backend_lmdb::get (nano::store::transaction const & transaction_a, wallet_handle const & wallet_a, nano::account const & account_a) const
{
	wallet_value result;
	nano::store::lmdb::db_val pub_key (account_a);
	auto mdb_pub_key = nano::store::lmdb::to_mdb_val (pub_key);
	MDB_val mdb_value{};
	auto status = mdb_get (environment.tx (transaction_a), table_handle (wallet_a), &mdb_pub_key, &mdb_value);
	if (status == 0)
	{
		result = wallet_value{ nano::store::lmdb::from_mdb_val (mdb_value) };
	}
	else
	{
		result.key.clear ();
		result.work = 0;
	}
	return result;
}

void wallets_backend_lmdb::put (nano::store::write_transaction const & transaction_a, wallet_handle const & wallet_a, nano::account const & account_a, wallet_value const & entry_a)
{
	nano::store::lmdb::db_val key (account_a);
	auto value = entry_a.val ();
	auto mdb_key = nano::store::lmdb::to_mdb_val (key);
	auto mdb_value = nano::store::lmdb::to_mdb_val (value);
	auto status = mdb_put (environment.tx (transaction_a), table_handle (wallet_a), &mdb_key, &mdb_value, 0);
	release_assert (nano::store::lmdb::success (status), nano::store::lmdb::error_string (status));
}

void wallets_backend_lmdb::del (nano::store::write_transaction const & transaction_a, wallet_handle const & wallet_a, nano::account const & account_a)
{
	nano::store::lmdb::db_val key (account_a);
	auto mdb_key = nano::store::lmdb::to_mdb_val (key);
	auto status = mdb_del (environment.tx (transaction_a), table_handle (wallet_a), &mdb_key, nullptr);
	release_assert (nano::store::lmdb::success (status), nano::store::lmdb::error_string (status));
}

auto wallets_backend_lmdb::begin (nano::store::transaction const & transaction_a, wallet_handle const & wallet_a, nano::account const & account_a) const -> iterator
{
	return iterator{ nano::store::iterator{ transaction_a, nano::store::lmdb::iterator::lower_bound (environment.tx (transaction_a), table_handle (wallet_a), nano::store::lmdb::to_mdb_val (account_a)) } };
}

auto wallets_backend_lmdb::end (nano::store::transaction const & transaction_a, wallet_handle const & wallet_a) const -> iterator
{
	return iterator{ nano::store::iterator{ transaction_a, nano::store::lmdb::iterator::end (environment.tx (transaction_a), table_handle (wallet_a)) } };
}

std::optional<nano::block_hash> wallets_backend_lmdb::send_action_id_get (nano::store::transaction const & transaction_a, std::string_view id_a) const
{
	nano::store::lmdb::db_val id_val{ id_a.size (), id_a.data () };
	auto mdb_id_key = nano::store::lmdb::to_mdb_val (id_val);
	MDB_val mdb_result{};
	auto status = mdb_get (environment.tx (transaction_a), send_action_ids_handle, &mdb_id_key, &mdb_result);
	if (status == MDB_NOTFOUND)
	{
		return std::nullopt;
	}
	release_assert (nano::store::lmdb::success (status), nano::store::lmdb::error_string (status));
	return nano::block_hash{ nano::store::lmdb::from_mdb_val (mdb_result) };
}

bool wallets_backend_lmdb::send_action_id_put (nano::store::write_transaction const & transaction_a, std::string_view id_a, nano::block_hash const & hash_a)
{
	nano::store::lmdb::db_val id_val{ id_a.size (), id_a.data () };
	nano::store::lmdb::db_val hash_val{ hash_a };
	auto mdb_id_key = nano::store::lmdb::to_mdb_val (id_val);
	auto mdb_hash_val = nano::store::lmdb::to_mdb_val (hash_val);
	auto status = mdb_put (environment.tx (transaction_a), send_action_ids_handle, &mdb_id_key, &mdb_hash_val, 0);
	return status == 0;
}

void wallets_backend_lmdb::clear_send_action_ids (nano::store::write_transaction const & transaction_a)
{
	auto status = mdb_drop (environment.tx (transaction_a), send_action_ids_handle, 0);
	release_assert (nano::store::lmdb::success (status), nano::store::lmdb::error_string (status));
}

std::filesystem::path wallets_backend_lmdb::path () const
{
	return environment.database_path;
}

void wallets_backend_lmdb::backup (nano::logger & logger_a)
{
	environment.create_backup_file (environment.database_path, logger_a);
}

auto wallets_backend_lmdb::table_handle (wallet_handle const & wallet_a) const -> MDB_dbi
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	auto existing = wallet_handles.find (wallet_a.id);
	release_assert (existing != wallet_handles.end (), "wallet table is not opened: {}", wallet_a.id.to_string ());
	return existing->second;
}

auto wallets_backend_lmdb::open_wallet_handle (nano::store::transaction const & transaction_a, nano::wallet_id const & wallet_id_a, bool create_a) -> MDB_dbi
{
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		auto existing = wallet_handles.find (wallet_id_a);
		if (existing != wallet_handles.end ())
		{
			return existing->second;
		}
	}

	MDB_dbi handle_l;
	auto flags = create_a ? MDB_CREATE : 0;
	auto name = wallet_id_a.to_string ();
	auto error = mdb_dbi_open (environment.tx (transaction_a), name.c_str (), flags, &handle_l);
	if (error != 0)
	{
		throw std::runtime_error ("Failed to open wallet database '" + name + "': " + nano::store::lmdb::error_string (error));
	}

	nano::lock_guard<nano::mutex> guard{ mutex };
	auto [it, inserted] = wallet_handles.emplace (wallet_id_a, handle_l);
	if (!inserted)
	{
		it->second = handle_l;
	}
	return it->second;
}
}
