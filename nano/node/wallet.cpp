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

template class nano::store::typed_iterator<nano::account, nano::wallet::wallet_value>;

namespace nano::wallet
{
/*
 * wallet_cipher
 */

wallet_cipher::wallet_cipher (wallet_store const & issuer, nano::raw_key wallet_key) :
	issuer{ &issuer },
	wallet_key{ wallet_key }
{
}

nano::raw_key wallet_cipher::encrypt (nano::raw_key const & plaintext, nano::uint128_union const & iv) const
{
	nano::raw_key result;
	result.encrypt (plaintext, wallet_key, iv);
	return result;
}

nano::raw_key wallet_cipher::decrypt (nano::uint256_union const & ciphertext, nano::uint128_union const & iv) const
{
	nano::raw_key result;
	result.decrypt (ciphertext, wallet_key, iv);
	return result;
}

nano::raw_key wallet_cipher::reseal (nano::raw_key const & new_password_key, nano::uint128_union const & iv) const
{
	nano::raw_key result;
	result.encrypt (wallet_key, new_password_key, iv);
	return result;
}

/*
 * wallet_store
 */

// Wallet version number
nano::account const wallet_store::version_special{};
// Random number used to salt private key encryption
nano::account const wallet_store::salt_special (1);
// Key used to encrypt wallet keys, encrypted itself by the user password
nano::account const wallet_store::wallet_key_special (2);
// Check value used to see if password is valid
nano::account const wallet_store::check_special (3);
// Representative account to be used if we open a new account
nano::account const wallet_store::representative_special (4);
// Wallet seed for deterministic key generation
nano::account const wallet_store::seed_special (5);
// Current key index for deterministic keys
nano::account const wallet_store::deterministic_index_special (6);
int const wallet_store::special_count (7);
std::string const wallet_store::default_password{ "" };
std::size_t const wallet_store::check_iv_index (0);
std::size_t const wallet_store::seed_iv_index (1);

wallet_store::wallet_store (nano::kdf & kdf_a, nano::store::write_transaction & transaction_a, nano::wallet::wallets_backend & backend_a, unsigned fanout_a, std::string const & wallet_a, std::string const & json_a) :
	password (0, fanout_a),
	wallet_key_mem (0, fanout_a),
	kdf (kdf_a),
	backend{ backend_a }
{
	handle = backend.wallet_open_or_create (transaction_a, wallet_a);
	try
	{
		release_assert (!backend.entry_exists (transaction_a, handle, version_special), "wallet already exists before import");
		boost::property_tree::ptree wallet_l;
		std::stringstream istream (json_a);
		try
		{
			boost::property_tree::read_json (istream, wallet_l);
		}
		catch (...)
		{
			throw std::runtime_error ("Failed to parse wallet JSON");
		}
		for (auto i (wallet_l.begin ()), n (wallet_l.end ()); i != n; ++i)
		{
			nano::account key;
			if (key.decode_hex (i->first))
			{
				throw std::runtime_error ("Failed to decode wallet key hex");
			}
			nano::raw_key value;
			if (value.decode_hex (wallet_l.get<std::string> (i->first)))
			{
				throw std::runtime_error ("Failed to decode wallet value hex");
			}
			entry_put_raw (transaction_a, key, nano::wallet::wallet_value (value, 0));
		}
		bool missing = false;
		missing |= !backend.entry_exists (transaction_a, handle, version_special);
		missing |= !backend.entry_exists (transaction_a, handle, wallet_key_special);
		missing |= !backend.entry_exists (transaction_a, handle, salt_special);
		missing |= !backend.entry_exists (transaction_a, handle, check_special);
		missing |= !backend.entry_exists (transaction_a, handle, representative_special);
		if (missing)
		{
			throw std::runtime_error ("Wallet is missing required entries");
		}
		nano::raw_key key;
		key.clear ();
		password.value_set (key);
		key = entry_get_raw (transaction_a, wallet_store::wallet_key_special).key;
		wallet_key_mem.value_set (key);
		attempt_password (transaction_a, default_password);
	}
	catch (...)
	{
		destroy (transaction_a);
		throw;
	}
}

wallet_store::wallet_store (nano::kdf & kdf_a, nano::store::write_transaction & transaction_a, nano::wallet::wallets_backend & backend_a, nano::account representative_a, unsigned fanout_a, std::string const & wallet_a) :
	password (0, fanout_a),
	wallet_key_mem (0, fanout_a),
	kdf (kdf_a),
	backend{ backend_a }
{
	handle = backend.wallet_open_or_create (transaction_a, wallet_a);
	try
	{
		if (!backend.entry_exists (transaction_a, handle, version_special))
		{
			version_put (transaction_a, version_current);
			nano::raw_key salt_l;
			random_pool::generate_block (salt_l.bytes.data (), salt_l.bytes.size ());
			entry_put_raw (transaction_a, wallet_store::salt_special, nano::wallet::wallet_value (salt_l, 0));
			// Wallet key is a fixed random key that encrypts all entries
			nano::raw_key wallet_key;
			random_pool::generate_block (wallet_key.bytes.data (), sizeof (wallet_key.bytes));
			auto password_l = derive_key (transaction_a, default_password);
			password.value_set (password_l);
			// Wallet key is encrypted by the user's password
			nano::raw_key encrypted;
			encrypted.encrypt (wallet_key, password_l, salt_l.owords[0]);
			entry_put_raw (transaction_a, wallet_store::wallet_key_special, nano::wallet::wallet_value (encrypted, 0));
			nano::raw_key wallet_key_enc;
			wallet_key_enc = encrypted;
			wallet_key_mem.value_set (wallet_key_enc);
			nano::raw_key zero;
			zero.clear ();
			nano::raw_key check;
			check.encrypt (zero, wallet_key, salt_l.owords[check_iv_index]);
			entry_put_raw (transaction_a, wallet_store::check_special, nano::wallet::wallet_value (check, 0));
			nano::raw_key rep;
			rep.bytes = representative_a.bytes;
			entry_put_raw (transaction_a, wallet_store::representative_special, nano::wallet::wallet_value (rep, 0));
			nano::raw_key seed;
			random_pool::generate_block (seed.bytes.data (), seed.bytes.size ());
			// The freshly generated wallet key needs no unlock to be known valid
			seed_set (transaction_a, nano::wallet::wallet_cipher{ *this, wallet_key }, seed);
			entry_put_raw (transaction_a, wallet_store::deterministic_index_special, nano::wallet::wallet_value (0, 0));
		}
		nano::raw_key key;
		key = entry_get_raw (transaction_a, wallet_store::wallet_key_special).key;
		wallet_key_mem.value_set (key);
		attempt_password (transaction_a, default_password);
	}
	catch (...)
	{
		destroy (transaction_a);
		throw;
	}
}

std::vector<nano::account> wallet_store::accounts (nano::store::transaction const & transaction_a) const
{
	std::vector<nano::account> result;
	for (auto i (begin (transaction_a)), n (end (transaction_a)); i != n; ++i)
	{
		nano::account const & account (i->first);
		result.push_back (account);
	}
	return result;
}

void wallet_store::destroy (nano::store::write_transaction const & transaction_a)
{
	backend.wallet_drop (transaction_a, handle.lock ().get ());
}

bool wallet_store::is_representative (nano::store::transaction const & transaction_a) const
{
	return exists (transaction_a, representative (transaction_a));
}

void wallet_store::representative_set (nano::store::write_transaction const & transaction_a, nano::account const & representative_a)
{
	nano::raw_key rep;
	rep.bytes = representative_a.bytes;
	entry_put_raw (transaction_a, wallet_store::representative_special, nano::wallet::wallet_value (rep, 0));
}

nano::account wallet_store::representative (nano::store::transaction const & transaction_a) const
{
	nano::wallet::wallet_value value (entry_get_raw (transaction_a, wallet_store::representative_special));
	return reinterpret_cast<nano::account const &> (value.key);
}

nano::public_key wallet_store::insert_adhoc (nano::store::write_transaction const & transaction, nano::wallet::wallet_cipher const & cipher, nano::raw_key const & prv)
{
	release_assert (cipher.issuer == this);
	nano::public_key pub (nano::pub_key (prv));
	auto ciphertext = cipher.encrypt (prv, pub.owords[0]);
	entry_put_raw (transaction, pub, nano::wallet::wallet_value (ciphertext, 0));
	return pub;
}

bool wallet_store::insert_watch (nano::store::write_transaction const & transaction_a, nano::account const & pub_a)
{
	bool error (!valid_public_key (pub_a));
	if (!error)
	{
		entry_put_raw (transaction_a, pub_a, nano::wallet::wallet_value (nano::raw_key (0), 0));
	}
	return error;
}

void wallet_store::erase (nano::store::write_transaction const & transaction_a, nano::account const & pub)
{
	backend.entry_del (transaction_a, handle, pub);
}

nano::wallet::wallet_value wallet_store::entry_get_raw (nano::store::transaction const & transaction_a, nano::account const & pub_a) const
{
	auto value = backend.entry_get (transaction_a, handle, pub_a);
	if (value)
	{
		return nano::wallet::wallet_value{ *value };
	}
	nano::wallet::wallet_value result;
	result.key.clear ();
	result.work = 0;
	return result;
}

void wallet_store::entry_put_raw (nano::store::write_transaction const & transaction_a, nano::account const & pub_a, nano::wallet::wallet_value const & entry_a)
{
	backend.entry_put (transaction_a, handle, pub_a, entry_a);
}

nano::wallet::key_type wallet_store::key_type (nano::wallet::wallet_value const & value_a) const
{
	auto number (value_a.key.number ());
	nano::wallet::key_type result;
	auto text (number.convert_to<std::string> ());
	if (number > std::numeric_limits<uint64_t>::max ())
	{
		result = key_type::adhoc;
	}
	else
	{
		if ((number >> 32).convert_to<uint32_t> () == 1)
		{
			result = key_type::deterministic;
		}
		else
		{
			result = key_type::unknown;
		}
	}
	return result;
}

nano::result<nano::raw_key> wallet_store::fetch (nano::store::transaction const & transaction, nano::account const & pub) const
{
	auto cipher = unlock (transaction);
	if (!cipher)
	{
		return nano::error (nano::error_common::wallet_locked);
	}
	return fetch (transaction, cipher.value (), pub);
}

nano::result<nano::raw_key> wallet_store::fetch (nano::store::transaction const & transaction, nano::wallet::wallet_cipher const & cipher, nano::account const & pub) const
{
	release_assert (cipher.issuer == this);
	auto value = entry_get_raw (transaction, pub);
	if (value.key.is_zero ())
	{
		return nano::error (nano::error_common::account_not_found_wallet);
	}

	nano::raw_key prv;
	switch (key_type (value))
	{
		case key_type::deterministic:
		{
			auto seed_l = seed_decrypt (transaction, cipher);
			auto index = static_cast<uint32_t> (value.key.number () & static_cast<uint32_t> (-1));
			prv = nano::deterministic_key (seed_l, index);
			break;
		}
		case key_type::adhoc:
		{
			prv = cipher.decrypt (value.key, pub.owords[0]);
			break;
		}
		default:
		{
			return nano::error (nano::error_common::bad_private_key);
		}
	}

	// Verify the key
	nano::public_key compare = nano::pub_key (prv);
	if (pub != compare)
	{
		return nano::error (nano::error_common::bad_private_key);
	}

	return prv;
}

bool wallet_store::valid_public_key (nano::public_key const & pub) const
{
	return pub.number () >= special_count;
}

bool wallet_store::exists (nano::store::transaction const & transaction_a, nano::account const & pub) const
{
	return valid_public_key (pub) && find (transaction_a, pub) != end (transaction_a);
}

void wallet_store::serialize_json (nano::store::transaction const & transaction_a, std::string & string_a) const
{
	boost::property_tree::ptree tree;
	// Iterate from account 0 to include the specials slots in the serialized output.
	for (iterator i{ backend.entries_begin (transaction_a, handle) }, n{ backend.entries_end (transaction_a, handle) }; i != n; ++i)
	{
		tree.put (i->first.to_string (), i->second.key.to_string ());
	}
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree);
	string_a = ostream.str ();
}

void wallet_store::write_backup (nano::store::transaction const & transaction_a, std::filesystem::path const & path_a) const
{
	std::ofstream backup_file;
	backup_file.open (path_a.string ());
	if (!backup_file.fail ())
	{
		// Set permissions to 600
		boost::system::error_code ec;
		nano::set_secure_perm_file (path_a, ec);

		std::string json;
		serialize_json (transaction_a, json);
		backup_file << json;
	}
}

nano::result<bool> wallet_store::move (nano::store::write_transaction const & transaction, wallet_store & other, std::vector<nano::public_key> const & keys)
{
	// Unlock both stores once so a concurrent password change cannot interrupt the transfer midway
	auto cipher = unlock (transaction);
	auto other_cipher = other.unlock (transaction);
	if (!cipher || !other_cipher)
	{
		return nano::error (nano::error_common::wallet_locked);
	}

	bool error = false;
	for (auto i (keys.begin ()), n (keys.end ()); i != n; ++i)
	{
		auto prv_result = other.fetch (transaction, other_cipher.value (), *i);
		if (prv_result)
		{
			insert_adhoc (transaction, cipher.value (), prv_result.value ());
			other.erase (transaction, *i);
		}
		else
		{
			error = true;
		}
	}
	return error;
}

nano::result<bool> wallet_store::import (nano::store::write_transaction const & transaction, wallet_store & other)
{
	// Unlock both stores once so a concurrent password change cannot interrupt the transfer midway
	auto cipher = unlock (transaction);
	auto other_cipher = other.unlock (transaction);
	if (!cipher || !other_cipher)
	{
		return nano::error (nano::error_common::wallet_locked);
	}

	bool error = false;
	for (auto i (other.begin (transaction)), n (other.end (transaction)); i != n; ++i)
	{
		auto prv_result = other.fetch (transaction, other_cipher.value (), i->first);
		if (prv_result)
		{
			if (!prv_result.value ().is_zero ())
			{
				insert_adhoc (transaction, cipher.value (), prv_result.value ());
			}
			else
			{
				insert_watch (transaction, i->first);
			}
			other.erase (transaction, i->first);
		}
		else
		{
			error = true;
		}
	}
	return error;
}

std::optional<uint64_t> wallet_store::work_get (nano::store::transaction const & transaction, nano::public_key const & pub) const
{
	auto entry = entry_get_raw (transaction, pub);
	if (!entry.key.is_zero ())
	{
		return entry.work;
	}
	return std::nullopt;
}

void wallet_store::work_put (nano::store::write_transaction const & transaction_a, nano::public_key const & pub_a, uint64_t work_a)
{
	auto entry (entry_get_raw (transaction_a, pub_a));
	debug_assert (!entry.key.is_zero ());
	entry.work = work_a;
	entry_put_raw (transaction_a, pub_a, entry);
}

unsigned wallet_store::version (nano::store::transaction const & transaction_a) const
{
	nano::wallet::wallet_value value (entry_get_raw (transaction_a, wallet_store::version_special));
	auto entry (value.key);
	auto result (static_cast<unsigned> (entry.bytes[31]));
	return result;
}

void wallet_store::version_put (nano::store::write_transaction const & transaction_a, unsigned version_a)
{
	nano::raw_key entry (version_a);
	entry_put_raw (transaction_a, wallet_store::version_special, nano::wallet::wallet_value (entry, 0));
}

nano::uint256_union wallet_store::check_value_get (nano::store::transaction const & transaction_a) const
{
	auto value = entry_get_raw (transaction_a, wallet_store::check_special);
	return value.key;
}

nano::uint256_union wallet_store::salt_get (nano::store::transaction const & transaction_a) const
{
	auto value = entry_get_raw (transaction_a, wallet_store::salt_special);
	return value.key;
}

std::optional<wallet_cipher> wallet_store::unlock (nano::store::transaction const & transaction_a) const
{
	auto const wallet_key_l = wallet_key_decrypt (transaction_a);
	nano::raw_key zero{};
	zero.clear ();
	nano::uint256_union check_l{};
	check_l.encrypt (zero, wallet_key_l, salt_get (transaction_a).owords[check_iv_index]);
	if (check_value_get (transaction_a) != check_l)
	{
		return std::nullopt;
	}
	return wallet_cipher{ *this, wallet_key_l };
}

nano::raw_key wallet_store::wallet_key_decrypt (nano::store::transaction const & transaction_a) const
{
	nano::lock_guard<std::recursive_mutex> lock{ mutex };
	nano::raw_key wallet_l;
	wallet_key_mem.value (wallet_l);
	nano::raw_key password_l;
	password.value (password_l);
	nano::raw_key result;
	result.decrypt (wallet_l, password_l, salt_get (transaction_a).owords[0]);
	return result;
}

nano::raw_key wallet_store::seed (nano::store::transaction const & transaction, nano::wallet::wallet_cipher const & cipher) const
{
	release_assert (cipher.issuer == this);
	return seed_decrypt (transaction, cipher);
}

nano::raw_key wallet_store::seed_decrypt (nano::store::transaction const & transaction, nano::wallet::wallet_cipher const & cipher) const
{
	auto encrypted_seed = entry_get_raw (transaction, wallet_store::seed_special).key;
	return cipher.decrypt (encrypted_seed, salt_get (transaction).owords[seed_iv_index]);
}

void wallet_store::seed_set (nano::store::write_transaction const & transaction, nano::wallet::wallet_cipher const & cipher, nano::raw_key const & seed)
{
	release_assert (cipher.issuer == this);
	auto ciphertext = cipher.encrypt (seed, salt_get (transaction).owords[seed_iv_index]);
	entry_put_raw (transaction, wallet_store::seed_special, nano::wallet::wallet_value (ciphertext, 0));
	deterministic_clear (transaction);
}

nano::public_key wallet_store::deterministic_insert (nano::store::write_transaction const & transaction, nano::wallet::wallet_cipher const & cipher)
{
	release_assert (cipher.issuer == this);
	auto index (deterministic_index_get (transaction));
	auto prv = deterministic_key (transaction, cipher, index);
	nano::public_key result (nano::pub_key (prv));
	// The indexed overload inserts without moving the cursor, so accounts may already exist at or above it; skip them rather than hand back an existing account
	while (exists (transaction, result))
	{
		++index;
		prv = deterministic_key (transaction, cipher, index);
		result = nano::pub_key (prv);
	}
	// A deterministic entry is tagged by a marker carrying its account index in the low 32 bits
	uint64_t marker (1);
	marker <<= 32;
	marker |= index;
	entry_put_raw (transaction, result, nano::wallet::wallet_value (marker, 0));
	++index;
	deterministic_index_set (transaction, index);
	return result;
}

// Random access counterpart of the allocating overload above: the cursor stays put, otherwise inserting a high index would strand every account below it
nano::public_key wallet_store::deterministic_insert (nano::store::write_transaction const & transaction, nano::wallet::wallet_cipher const & cipher, uint32_t const index)
{
	release_assert (cipher.issuer == this);
	auto prv = deterministic_key (transaction, cipher, index);
	nano::public_key result (nano::pub_key (prv));
	uint64_t marker (1);
	marker <<= 32;
	marker |= index;
	entry_put_raw (transaction, result, nano::wallet::wallet_value (marker, 0));
	return result;
}

nano::raw_key wallet_store::deterministic_key (nano::store::transaction const & transaction, nano::wallet::wallet_cipher const & cipher, uint32_t index) const
{
	release_assert (cipher.issuer == this);
	auto wallet_seed = seed_decrypt (transaction, cipher);
	return nano::deterministic_key (wallet_seed, index);
}

uint32_t wallet_store::deterministic_index_get (nano::store::transaction const & transaction_a) const
{
	nano::wallet::wallet_value value (entry_get_raw (transaction_a, wallet_store::deterministic_index_special));
	return static_cast<uint32_t> (value.key.number () & static_cast<uint32_t> (-1));
}

void wallet_store::deterministic_index_set (nano::store::write_transaction const & transaction_a, uint32_t index_a)
{
	nano::raw_key index_l (index_a);
	nano::wallet::wallet_value value (index_l, 0);
	entry_put_raw (transaction_a, wallet_store::deterministic_index_special, value);
}

void wallet_store::deterministic_clear (nano::store::write_transaction const & transaction_a)
{
	nano::uint256_union key (0);
	for (auto i (begin (transaction_a)), n (end (transaction_a)); i != n;)
	{
		switch (key_type (nano::wallet::wallet_value (i->second)))
		{
			case key_type::deterministic:
			{
				auto const & key (i->first);
				erase (transaction_a, key);
				i = begin (transaction_a, key);
				break;
			}
			default:
			{
				++i;
				break;
			}
		}
	}
	deterministic_index_set (transaction_a, 0);
}

bool wallet_store::valid_password (nano::store::transaction const & transaction_a) const
{
	return unlock (transaction_a).has_value ();
}

bool wallet_store::attempt_password (nano::store::transaction const & transaction_a, std::string const & password_a)
{
	bool result = false;
	{
		nano::lock_guard<std::recursive_mutex> lock{ mutex };
		auto password_l = derive_key (transaction_a, password_a);
		password.value_set (password_l);
		result = !valid_password (transaction_a);
	}
	if (!result)
	{
		switch (version (transaction_a))
		{
			case version_4:
				break;
			default:
				debug_assert (false);
		}
	}
	return result;
}

void wallet_store::password_clear ()
{
	// Take the store mutex so the clear cannot be overwritten by an in-flight rekey or attempt_password
	nano::lock_guard<std::recursive_mutex> lock{ mutex };
	nano::raw_key empty;
	empty.clear ();
	password.value_set (empty);
}

bool wallet_store::rekey (nano::store::write_transaction const & transaction_a, std::string const & password_a)
{
	nano::lock_guard<std::recursive_mutex> lock{ mutex };
	auto cipher = unlock (transaction_a);
	if (!cipher)
	{
		return true;
	}
	auto password_new = derive_key (transaction_a, password_a);
	auto encrypted = cipher.value ().reseal (password_new, salt_get (transaction_a).owords[0]);
	password.value_set (password_new);
	wallet_key_mem.value_set (encrypted);
	entry_put_raw (transaction_a, wallet_store::wallet_key_special, nano::wallet::wallet_value (encrypted, 0));
	return false;
}

nano::raw_key wallet_store::derive_key (nano::store::transaction const & transaction_a, std::string const & password_a) const
{
	auto const salt_l = salt_get (transaction_a);
	nano::raw_key result;
	kdf.phs (result, password_a, salt_l);
	return result;
}

auto wallet_store::begin (nano::store::transaction const & txn) const -> iterator
{
	return iterator{ backend.entries_begin (txn, handle, nano::account{ special_count }) };
}

auto wallet_store::begin (nano::store::transaction const & txn, nano::account const & key) const -> iterator
{
	return iterator{ backend.entries_begin (txn, handle, key) };
}

auto wallet_store::end (nano::store::transaction const & txn) const -> iterator
{
	return iterator{ backend.entries_end (txn, handle) };
}

auto wallet_store::find (nano::store::transaction const & txn, nano::account const & key) const -> iterator
{
	auto it = begin (txn, key);
	auto end_it = end (txn);
	if (it == end_it)
	{
		return end_it;
	}
	if (it->first == key)
	{
		return it;
	}
	return end_it;
}

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

nano::uint128_t const wallets::generate_priority = std::numeric_limits<nano::uint128_t>::max ();
nano::uint128_t const wallets::high_priority = std::numeric_limits<nano::uint128_t>::max () - 1;

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
	observer{ [] (bool) {} },
	kdf{ network_params.kdf_work },
	workers{ config.wallet_threads, nano::thread_role::name::wallet_worker, /* auto_start */ true },
	rep_tracker{ *this, node, ledger, config, network_params, stats, logger },
	receivable_tracker{ *this, node, ledger, config, network_params, stats, logger }
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
	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::wallet_actions);
		do_wallet_actions ();
	} };

	rep_tracker.start ();
	receivable_tracker.start ();
}

void wallets::stop ()
{
	{
		nano::lock_guard<nano::mutex> action_lock{ action_mutex };
		stopped = true;
		actions.clear ();
	}
	condition.notify_all ();

	if (thread.joinable ())
	{
		thread.join ();
	}

	rep_tracker.stop ();
	receivable_tracker.stop ();

	workers.stop ();
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

void wallets::queue_wallet_action (nano::uint128_t const & amount, std::shared_ptr<wallet> const & wallet_l, std::function<void (wallet &)> action)
{
	{
		nano::lock_guard<nano::mutex> action_lock{ action_mutex };
		actions.emplace (amount, std::make_pair (wallet_l, action));
	}
	condition.notify_all ();
}

void wallets::queue_wallet_action (nano::uint128_t const & amount, nano::wallet_id const & id, std::function<void (wallet &)> action)
{
	queue_wallet_action (amount, std::make_shared<wallet> (*this, id), std::move (action));
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

void wallets::do_wallet_actions ()
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
			// The wallet handle is id-keyed, an action against a destroyed wallet simply no-ops
			action_lock.unlock ();
			observer (true);
			current (*wallet);
			observer (false);
			action_lock.lock ();
		}
		else
		{
			condition.wait (action_lock);
		}
	}
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
		queue_wallet_action (high_priority, wallet_l.id, [] (wallet & wallet) {
			wallet.search_receivable ();
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
		work_ensure (wallet_l.id, key, key);
	}

	return key;
}

nano::public_key wallets::deterministic_insert_impl (wallet_data & wallet_l, nano::store::write_transaction const & transaction, nano::wallet::wallet_cipher const & cipher, uint32_t index, bool generate_work)
{
	auto key = wallet_l.store.deterministic_insert (transaction, cipher, index);

	logger.info (nano::log::type::wallet, "Deterministically inserted new account: {} with index: {}", key.to_account (), index);

	if (generate_work)
	{
		work_ensure (wallet_l.id, key, key);
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
		work_ensure (id, key, ledger.latest_root (ledger_txn, key));
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
			return nullptr;
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
	if (block != nullptr)
	{
		if (action_complete (id, block, account, generate_work, details))
		{
			// Return null block after work generation or ledger process error
			block = nullptr;
		}
	}
	return block;
}

std::shared_ptr<nano::block> wallets::change_action (nano::wallet_id const & id, nano::account const & source, nano::account const & representative, uint64_t work, bool generate_work)
{
	std::shared_ptr<nano::block> block;
	nano::block_details details;
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		auto wallet_l = find_wallet (id);
		if (!wallet_l)
		{
			logger.warn (nano::log::type::wallet, "Changing representative for account: {} failed, wallet no longer exists", source);
			return nullptr;
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
	if (block != nullptr)
	{
		if (action_complete (id, block, source, generate_work, details))
		{
			// Return null block after work generation or ledger process error
			block = nullptr;
		}
	}
	return block;
}

std::shared_ptr<nano::block> wallets::send_action (nano::wallet_id const & id, nano::account const & source, nano::account const & account, nano::uint128_t const & amount, uint64_t work, bool generate_work, std::optional<std::string> send_id)
{
	auto prepare_send = [this, &source, &amount, &work, &account, &send_id] (auto const & transaction, wallet_data & wallet_l) {
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
					network.flood_block (block, nano::transport::traffic_type::block_broadcast_initial);
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
			return nullptr;
		}
		result = prepare_send (transaction, *wallet_l);
	}
	else
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		auto wallet_l = find_wallet (id);
		if (!wallet_l)
		{
			logger.warn (nano::log::type::wallet, "Sending from account: {} failed, wallet no longer exists", source);
			return nullptr;
		}
		result = prepare_send (tx_begin_read (), *wallet_l);
	}

	std::shared_ptr<nano::block> block;
	bool error;
	bool cached_block;
	nano::block_details details;
	std::tie (block, error, cached_block, details) = result;

	if (!error && block != nullptr && !cached_block)
	{
		if (action_complete (id, block, source, generate_work, details))
		{
			// Return null block after work generation or ledger process error
			block = nullptr;
		}
	}
	return block;
}

bool wallets::action_complete (nano::wallet_id const & id, std::shared_ptr<nano::block> const & block, nano::account const & account, bool const generate_work, nano::block_details const & details)
{
	bool error{ false };
	// Unschedule any work caching for this account
	delayed_work->erase (account);
	if (block != nullptr)
	{
		auto required_difficulty{ network_params.work.threshold (block->work_version (), details) };
		if (network_params.work.difficulty (*block) < required_difficulty)
		{
			logger.info (nano::log::type::wallet, "Cached or provided work for block: {}, account {}: is invalid, regenerating...",
			block->hash (),
			account);

			debug_assert (required_difficulty <= node.max_work_generate_difficulty (block->work_version ()));
			error = !node.work_generate_blocking (*block, required_difficulty).has_value ();
		}
		if (!error)
		{
			auto result = node.process_local (block);
			error = !result || result.value () != nano::block_status::progress;
			debug_assert (error || block->sideband ().details == details);
		}
		if (!error && generate_work)
		{
			// Pregenerate work for next block based on the block just created
			work_ensure (id, account, block->hash ());
		}
	}
	return error;
}

bool wallets::change_sync (nano::wallet_id const & id, nano::account const & source, nano::account const & representative)
{
	std::promise<bool> result;
	std::future<bool> future = result.get_future ();
	change_async (
	id, source, representative, [&result] (std::shared_ptr<nano::block> const & block) {
		result.set_value (block == nullptr);
	},
	true);
	return future.get ();
}

void wallets::change_async (nano::wallet_id const & id, nano::account const & source, nano::account const & representative, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work, bool generate_work)
{
	queue_wallet_action (high_priority, id, [source, representative, action, work, generate_work] (wallet & wallet) {
		auto block (wallet.change_action (source, representative, work, generate_work));
		action (block);
	});
}

bool wallets::receive_sync (nano::wallet_id const & id, std::shared_ptr<nano::block> const & block, nano::account const & representative, nano::uint128_t const & amount)
{
	std::promise<bool> result;
	std::future<bool> future = result.get_future ();
	receive_async (
	id, block->hash (), representative, amount, block->destination (), [&result] (std::shared_ptr<nano::block> const & block) {
		result.set_value (block == nullptr);
	},
	true);
	return future.get ();
}

void wallets::receive_async (nano::wallet_id const & id, nano::block_hash const & hash, nano::account const & representative, nano::uint128_t const & amount, nano::account const & account, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work, bool generate_work)
{
	queue_wallet_action (amount, id, [hash, representative, amount, account, action, work, generate_work] (wallet & wallet) {
		auto block (wallet.receive_action (hash, representative, amount, account, work, generate_work));
		action (block);
	});
}

nano::block_hash wallets::send_sync (nano::wallet_id const & id, nano::account const & source, nano::account const & account, nano::uint128_t const & amount)
{
	std::promise<nano::block_hash> result;
	std::future<nano::block_hash> future = result.get_future ();
	send_async (
	id, source, account, amount, [&result] (std::shared_ptr<nano::block> const & block) {
		result.set_value (block->hash ());
	},
	true);
	return future.get ();
}

void wallets::send_async (nano::wallet_id const & id, nano::account const & source, nano::account const & account, nano::uint128_t const & amount, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work, bool generate_work, std::optional<std::string> send_id)
{
	queue_wallet_action (high_priority, id, [source, account, amount, action, work, generate_work, send_id] (wallet & wallet) {
		auto block (wallet.send_action (source, account, amount, work, generate_work, send_id));
		action (block);
	});
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
	if (node.work_generation_enabled ())
	{
		auto difficulty (node.default_difficulty (nano::work_version::work_1));
		auto opt_work_l (node.work_generate_blocking (nano::work_version::work_1, root, difficulty, account));
		if (opt_work_l.has_value ())
		{
			auto transaction = tx_begin_write ();
			nano::lock_guard<nano::mutex> lock{ mutex };
			auto wallet_l = find_wallet (id);
			if (wallet_l)
			{
				if (wallet_l->store.exists (transaction, account))
				{
					work_update_impl (*wallet_l, transaction, account, root, opt_work_l.value ());
				}
			}
		}
		else if (!node.stopped)
		{
			logger.warn (nano::log::type::wallet, "Could not precache work for root: {} due to work generation failure", root);
		}
	}
}

void wallets::work_ensure (nano::wallet_id const & id, nano::account const & account, nano::root const & root)
{
	using namespace std::chrono_literals;
	std::chrono::seconds const precache_delay = network_params.network.is_dev_network () ? 1s : 10s;

	delayed_work->operator[] (account) = root;

	workers.post_delayed (precache_delay, [this, id, account, root] {
		if (stopped)
		{
			return;
		}
		auto delayed_work_l = delayed_work.lock ();
		auto existing (delayed_work_l->find (account));
		if (existing != delayed_work_l->end () && existing->second == root)
		{
			delayed_work_l->erase (existing);
			queue_wallet_action (generate_priority, id, [account, root] (wallet & wallet) {
				wallet.work_cache_blocking (account, root);
			});
		}
	});
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
	info.put ("actions", actions.size ());
	info.add ("reps", rep_tracker.container_info ());
	return info;
}
}
