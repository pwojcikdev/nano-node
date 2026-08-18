#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/files.hpp>
#include <nano/lib/formatting.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/wallet/wallet_store.hpp>
#include <nano/store/typed_iterator_templ.hpp>
#include <nano/wallet/wallet_value.hpp>
#include <nano/wallet/wallets_backend.hpp>

#include <boost/property_tree/json_parser.hpp>

#include <stdexcept>

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
}
