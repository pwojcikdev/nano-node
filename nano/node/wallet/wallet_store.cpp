#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/files.hpp>
#include <nano/node/wallet/wallet_store.hpp>
#include <nano/store/typed_iterator_templ.hpp>

#include <boost/property_tree/json_parser.hpp>

#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

template class nano::store::typed_iterator<nano::account, nano::wallet_value>;

// Wallet version number
nano::account const nano::wallet_store::version_special{};
// Random number used to salt private key encryption
nano::account const nano::wallet_store::salt_special (1);
// Key used to encrypt wallet keys, encrypted itself by the user password
nano::account const nano::wallet_store::wallet_key_special (2);
// Check value used to see if password is valid
nano::account const nano::wallet_store::check_special (3);
// Representative account to be used if we open a new account
nano::account const nano::wallet_store::representative_special (4);
// Wallet seed for deterministic key generation
nano::account const nano::wallet_store::seed_special (5);
// Current key index for deterministic keys
nano::account const nano::wallet_store::deterministic_index_special (6);
int const nano::wallet_store::special_count (7);
std::string const nano::wallet_store::default_password{ "" };
std::size_t const nano::wallet_store::check_iv_index (0);
std::size_t const nano::wallet_store::seed_iv_index (1);

nano::wallet_store::wallet_store (nano::kdf & kdf, store::lmdb::env & env, unsigned fanout) :
	password{ 0, fanout },
	wallet_key_mem{ 0, fanout },
	kdf{ kdf },
	repository{ env }
{
}

void nano::wallet_store::ensure_required_entries (nano::store::transaction const & txn_wallet) const
{
	bool missing = false;
	missing |= !repository.exists (txn_wallet, version_special);
	missing |= !repository.exists (txn_wallet, wallet_key_special);
	missing |= !repository.exists (txn_wallet, salt_special);
	missing |= !repository.exists (txn_wallet, check_special);
	missing |= !repository.exists (txn_wallet, representative_special);
	if (missing)
	{
		throw std::runtime_error ("Wallet is missing required entries");
	}
}

void nano::wallet_store::open (nano::store::write_transaction const & txn_wallet, std::string const & wallet_path)
{
	repository.open (txn_wallet, wallet_path);
	ensure_required_entries (txn_wallet);

	auto wallet_key = repository.get_raw (txn_wallet, nano::wallet_store::wallet_key_special).key;
	wallet_key_mem.value_set (wallet_key);
	attempt_password (txn_wallet, default_password);
}

void nano::wallet_store::create (nano::store::write_transaction const & txn_wallet, nano::account representative, std::string const & wallet_path)
{
	repository.open (txn_wallet, wallet_path);

	if (repository.exists (txn_wallet, version_special))
	{
		throw std::runtime_error ("Wallet already exists");
	}

	try
	{
		version_put (txn_wallet, version_current);
		nano::raw_key salt;
		random_pool::generate_block (salt.bytes.data (), salt.bytes.size ());
		repository.put_raw (txn_wallet, nano::wallet_store::salt_special, nano::wallet_value (salt, 0));

		// Wallet key is a fixed random key that encrypts all entries
		nano::raw_key wallet_key;
		random_pool::generate_block (wallet_key.bytes.data (), sizeof (wallet_key.bytes));
		nano::raw_key password_key;
		derive_key (password_key, txn_wallet, default_password);
		password.value_set (password_key);

		// Wallet key is encrypted by the user's password
		nano::raw_key encrypted_wallet_key;
		encrypted_wallet_key.encrypt (wallet_key, password_key, salt.owords[0]);
		repository.put_raw (txn_wallet, nano::wallet_store::wallet_key_special, nano::wallet_value (encrypted_wallet_key, 0));
		wallet_key_mem.value_set (encrypted_wallet_key);

		nano::raw_key zero;
		zero.clear ();
		nano::raw_key check;
		check.encrypt (zero, wallet_key, salt.owords[check_iv_index]);
		repository.put_raw (txn_wallet, nano::wallet_store::check_special, nano::wallet_value (check, 0));

		nano::raw_key representative_key;
		representative_key.bytes = representative.bytes;
		repository.put_raw (txn_wallet, nano::wallet_store::representative_special, nano::wallet_value (representative_key, 0));

		nano::raw_key seed;
		random_pool::generate_block (seed.bytes.data (), seed.bytes.size ());
		seed_set (txn_wallet, seed);
		repository.put_raw (txn_wallet, nano::wallet_store::deterministic_index_special, nano::wallet_value (0, 0));

		ensure_required_entries (txn_wallet);

		attempt_password (txn_wallet, default_password);
	}
	catch (...)
	{
		repository.destroy (txn_wallet);
		throw;
	}
}

void nano::wallet_store::create_from_json (nano::store::write_transaction const & txn_wallet, std::string const & wallet_path, std::string const & json)
{
	repository.open (txn_wallet, wallet_path);

	if (repository.exists (txn_wallet, version_special))
	{
		throw std::runtime_error ("Wallet already exists");
	}

	try
	{
		boost::property_tree::ptree wallet_tree;
		std::stringstream stream (json);
		try
		{
			boost::property_tree::read_json (stream, wallet_tree);
		}
		catch (...)
		{
			throw std::runtime_error ("Failed to parse wallet JSON");
		}

		for (auto i = wallet_tree.begin (), n = wallet_tree.end (); i != n; ++i)
		{
			nano::account key;
			if (key.decode_hex (i->first))
			{
				throw std::runtime_error ("Failed to decode wallet key hex");
			}

			nano::raw_key value;
			if (value.decode_hex (wallet_tree.get<std::string> (i->first)))
			{
				throw std::runtime_error ("Failed to decode wallet value hex");
			}

			repository.put_raw (txn_wallet, key, nano::wallet_value (value, 0));
		}

		ensure_required_entries (txn_wallet);

		nano::raw_key key;
		key.clear ();
		password.value_set (key);
		key = repository.get_raw (txn_wallet, nano::wallet_store::wallet_key_special).key;
		wallet_key_mem.value_set (key);
		attempt_password (txn_wallet, default_password);
	}
	catch (...)
	{
		repository.destroy (txn_wallet);
		throw;
	}
}

std::vector<nano::account> nano::wallet_store::accounts (nano::store::transaction const & txn_wallet) const
{
	std::vector<nano::account> result;
	for (auto i = begin (txn_wallet), n = end (txn_wallet); i != n; ++i)
	{
		result.push_back (i->first);
	}
	return result;
}

bool nano::wallet_store::is_open () const
{
	return repository.is_open ();
}

bool nano::wallet_store::is_representative (nano::store::transaction const & txn_wallet) const
{
	return exists (txn_wallet, representative (txn_wallet));
}

void nano::wallet_store::representative_set (nano::store::write_transaction const & txn_wallet, nano::account const & representative)
{
	nano::raw_key rep;
	rep.bytes = representative.bytes;
	repository.put_raw (txn_wallet, nano::wallet_store::representative_special, nano::wallet_value (rep, 0));
}

nano::account nano::wallet_store::representative (nano::store::transaction const & txn_wallet) const
{
	auto value = repository.get_raw (txn_wallet, nano::wallet_store::representative_special);
	return reinterpret_cast<nano::account const &> (value.key);
}

nano::public_key nano::wallet_store::insert_adhoc (nano::store::write_transaction const & txn_wallet, nano::raw_key const & prv)
{
	release_assert (valid_password (txn_wallet), "wallet is locked or password is invalid");
	nano::public_key pub (nano::pub_key (prv));
	nano::raw_key password_key;
	wallet_key (password_key, txn_wallet);
	nano::raw_key ciphertext;
	ciphertext.encrypt (prv, password_key, pub.owords[0].number ());
	repository.put_raw (txn_wallet, pub, nano::wallet_value (ciphertext, 0));
	return pub;
}

bool nano::wallet_store::insert_watch (nano::store::write_transaction const & txn_wallet, nano::account const & account)
{
	bool error (!valid_public_key (account));
	if (!error)
	{
		repository.put_raw (txn_wallet, account, nano::wallet_value (nano::raw_key (0), 0));
	}
	return error;
}

void nano::wallet_store::erase (nano::store::write_transaction const & txn_wallet, nano::account const & account)
{
	repository.erase (txn_wallet, account);
}

nano::key_type nano::wallet_store::key_type (nano::store::transaction const & txn_wallet, nano::account const & account) const
{
	return key_type (repository.get_raw (txn_wallet, account));
}

nano::key_type nano::wallet_store::key_type (nano::wallet_value const & value) const
{
	auto number = value.key.number ();
	if (number > std::numeric_limits<uint64_t>::max ())
	{
		return nano::key_type::adhoc;
	}

	if ((number >> 32).convert_to<uint32_t> () == 1)
	{
		return nano::key_type::deterministic;
	}

	return nano::key_type::unknown;
}

nano::result<nano::raw_key> nano::wallet_store::fetch (nano::store::transaction const & txn_wallet, nano::account const & account) const
{
	if (!valid_password (txn_wallet))
	{
		return nano::error (nano::error_common::wallet_locked);
	}

	auto value = repository.get_raw (txn_wallet, account);
	if (value.key.is_zero ())
	{
		return nano::error (nano::error_common::account_not_found_wallet);
	}

	nano::raw_key prv;
	switch (key_type (value))
	{
		case nano::key_type::deterministic:
		{
			auto index = static_cast<uint32_t> (value.key.number () & static_cast<uint32_t> (-1));
			prv = deterministic_key (txn_wallet, index);
			break;
		}
		case nano::key_type::adhoc:
		{
			nano::raw_key password_key;
			wallet_key (password_key, txn_wallet);
			prv.decrypt (value.key, password_key, account.owords[0].number ());
			break;
		}
		default:
		{
			return nano::error (nano::error_common::bad_private_key);
		}
	}

	if (account != nano::pub_key (prv))
	{
		return nano::error (nano::error_common::bad_private_key);
	}

	return prv;
}

bool nano::wallet_store::valid_public_key (nano::public_key const & pub) const
{
	return pub.number () >= special_count;
}

bool nano::wallet_store::exists (nano::store::transaction const & txn_wallet, nano::account const & account) const
{
	return valid_public_key (account) && find (txn_wallet, account) != end (txn_wallet);
}

std::string nano::wallet_store::serialize_json (nano::store::transaction const & txn_wallet) const
{
	return repository.serialize_json (txn_wallet);
}

void nano::wallet_store::write_backup (nano::store::transaction const & txn_wallet, std::filesystem::path const & path) const
{
	std::ofstream backup_file;
	backup_file.open (path.string ());
	if (!backup_file.fail ())
	{
		boost::system::error_code ec;
		nano::set_secure_perm_file (path, ec);
		backup_file << serialize_json (txn_wallet);
	}
}

bool nano::wallet_store::move (nano::store::write_transaction const & txn_wallet, nano::wallet_store & source, std::vector<nano::public_key> const & keys)
{
	release_assert (valid_password (txn_wallet), "wallet is locked or password is invalid");
	release_assert (source.valid_password (txn_wallet), "other wallet is locked or password is invalid");

	bool error = false;
	for (auto i = keys.begin (), n = keys.end (); i != n; ++i)
	{
		auto prv_result = source.fetch (txn_wallet, *i);
		if (prv_result)
		{
			insert_adhoc (txn_wallet, prv_result.value ());
			source.erase (txn_wallet, *i);
		}
		else
		{
			error = true;
		}
	}
	return error;
}

bool nano::wallet_store::import (nano::store::write_transaction const & txn_wallet, nano::wallet_store & source)
{
	release_assert (valid_password (txn_wallet), "wallet is locked or password is invalid");
	release_assert (source.valid_password (txn_wallet), "other wallet is locked or password is invalid");

	bool error = false;
	for (auto i = source.begin (txn_wallet), n = source.end (txn_wallet); i != n; ++i)
	{
		auto prv_result = source.fetch (txn_wallet, i->first);
		if (prv_result)
		{
			if (!prv_result.value ().is_zero ())
			{
				insert_adhoc (txn_wallet, prv_result.value ());
			}
			else
			{
				insert_watch (txn_wallet, i->first);
			}
			source.erase (txn_wallet, i->first);
		}
		else
		{
			error = true;
		}
	}
	return error;
}

std::optional<uint64_t> nano::wallet_store::work_get (nano::store::transaction const & txn_wallet, nano::public_key const & account) const
{
	auto entry = repository.get_raw (txn_wallet, account);
	if (!entry.key.is_zero ())
	{
		return entry.work;
	}
	return std::nullopt;
}

void nano::wallet_store::work_put (nano::store::write_transaction const & txn_wallet, nano::public_key const & account, uint64_t work)
{
	auto entry = repository.get_raw (txn_wallet, account);
	debug_assert (!entry.key.is_zero ());
	entry.work = work;
	repository.put_raw (txn_wallet, account, entry);
}

unsigned nano::wallet_store::version (nano::store::transaction const & txn_wallet) const
{
	auto value = repository.get_raw (txn_wallet, nano::wallet_store::version_special);
	return static_cast<unsigned> (value.key.bytes[31]);
}

void nano::wallet_store::version_put (nano::store::write_transaction const & txn_wallet, unsigned version)
{
	nano::raw_key entry (version);
	repository.put_raw (txn_wallet, nano::wallet_store::version_special, nano::wallet_value (entry, 0));
}

nano::uint256_union nano::wallet_store::check (nano::store::transaction const & txn_wallet) const
{
	return repository.get_raw (txn_wallet, nano::wallet_store::check_special).key;
}

nano::uint256_union nano::wallet_store::salt (nano::store::transaction const & txn_wallet) const
{
	return repository.get_raw (txn_wallet, nano::wallet_store::salt_special).key;
}

void nano::wallet_store::wallet_key (nano::raw_key & result, nano::store::transaction const & txn_wallet) const
{
	nano::lock_guard<std::recursive_mutex> lock{ mutex };
	nano::raw_key encrypted_wallet_key;
	wallet_key_mem.value (encrypted_wallet_key);
	nano::raw_key password_key;
	password.value (password_key);
	result.decrypt (encrypted_wallet_key, password_key, salt (txn_wallet).owords[0]);
}

nano::raw_key nano::wallet_store::seed (nano::store::transaction const & txn_wallet) const
{
	release_assert (valid_password (txn_wallet), "wallet is locked or password is invalid");
	auto value = repository.get_raw (txn_wallet, nano::wallet_store::seed_special);
	nano::raw_key wallet_key;
	this->wallet_key (wallet_key, txn_wallet);
	nano::raw_key result;
	result.decrypt (value.key, wallet_key, salt (txn_wallet).owords[seed_iv_index]);
	return result;
}

void nano::wallet_store::seed_set (nano::store::write_transaction const & txn_wallet, nano::raw_key const & seed)
{
	nano::raw_key wallet_key;
	this->wallet_key (wallet_key, txn_wallet);
	nano::raw_key ciphertext;
	ciphertext.encrypt (seed, wallet_key, salt (txn_wallet).owords[seed_iv_index]);
	repository.put_raw (txn_wallet, nano::wallet_store::seed_special, nano::wallet_value (ciphertext, 0));
	deterministic_clear (txn_wallet);
}

nano::public_key nano::wallet_store::deterministic_insert (nano::store::write_transaction const & txn_wallet)
{
	auto index = deterministic_index_get (txn_wallet);
	auto prv = deterministic_key (txn_wallet, index);
	nano::public_key result (nano::pub_key (prv));
	while (exists (txn_wallet, result))
	{
		++index;
		prv = deterministic_key (txn_wallet, index);
		result = nano::pub_key (prv);
	}
	uint64_t marker = 1;
	marker <<= 32;
	marker |= index;
	repository.put_raw (txn_wallet, result, nano::wallet_value (marker, 0));
	++index;
	deterministic_index_set (txn_wallet, index);
	return result;
}

nano::public_key nano::wallet_store::deterministic_insert (nano::store::write_transaction const & txn_wallet, uint32_t index)
{
	auto prv = deterministic_key (txn_wallet, index);
	nano::public_key result (nano::pub_key (prv));
	uint64_t marker = 1;
	marker <<= 32;
	marker |= index;
	repository.put_raw (txn_wallet, result, nano::wallet_value (marker, 0));
	return result;
}

nano::raw_key nano::wallet_store::deterministic_key (nano::store::transaction const & txn_wallet, uint32_t index) const
{
	release_assert (valid_password (txn_wallet), "wallet is locked or password is invalid");
	return nano::deterministic_key (seed (txn_wallet), index);
}

uint32_t nano::wallet_store::deterministic_index_get (nano::store::transaction const & txn_wallet) const
{
	auto value = repository.get_raw (txn_wallet, nano::wallet_store::deterministic_index_special);
	return static_cast<uint32_t> (value.key.number () & static_cast<uint32_t> (-1));
}

void nano::wallet_store::deterministic_index_set (nano::store::write_transaction const & txn_wallet, uint32_t index)
{
	nano::raw_key index_key (index);
	repository.put_raw (txn_wallet, nano::wallet_store::deterministic_index_special, nano::wallet_value (index_key, 0));
}

void nano::wallet_store::deterministic_clear (nano::store::write_transaction const & txn_wallet)
{
	for (auto i = begin (txn_wallet), n = end (txn_wallet); i != n;)
	{
		switch (key_type (i->second))
		{
			case nano::key_type::deterministic:
			{
				auto const & key = i->first;
				erase (txn_wallet, key);
				i = begin (txn_wallet, key);
				break;
			}
			default:
			{
				++i;
				break;
			}
		}
	}
	deterministic_index_set (txn_wallet, 0);
}

bool nano::wallet_store::valid_password (nano::store::transaction const & txn_wallet) const
{
	nano::raw_key zero;
	zero.clear ();
	nano::raw_key wallet_key;
	this->wallet_key (wallet_key, txn_wallet);
	nano::uint256_union encrypted_check;
	encrypted_check.encrypt (zero, wallet_key, salt (txn_wallet).owords[check_iv_index]);
	return check (txn_wallet) == encrypted_check;
}

bool nano::wallet_store::attempt_password (nano::store::transaction const & txn_wallet, std::string const & password_text)
{
	bool result = false;
	{
		nano::lock_guard<std::recursive_mutex> lock{ mutex };
		nano::raw_key password_key;
		derive_key (password_key, txn_wallet, password_text);
		password.value_set (password_key);
		result = !valid_password (txn_wallet);
	}
	if (!result)
	{
		switch (version (txn_wallet))
		{
			case version_4:
				break;
			default:
				debug_assert (false);
		}
	}
	return result;
}

bool nano::wallet_store::rekey (nano::store::write_transaction const & txn_wallet, std::string const & password_text)
{
	nano::lock_guard<std::recursive_mutex> lock{ mutex };
	bool result = false;
	if (valid_password (txn_wallet))
	{
		nano::raw_key password_new;
		derive_key (password_new, txn_wallet, password_text);
		nano::raw_key wallet_key;
		this->wallet_key (wallet_key, txn_wallet);
		password.value_set (password_new);
		nano::raw_key encrypted_wallet_key;
		encrypted_wallet_key.encrypt (wallet_key, password_new, salt (txn_wallet).owords[0]);
		wallet_key_mem.value_set (encrypted_wallet_key);
		repository.put_raw (txn_wallet, nano::wallet_store::wallet_key_special, nano::wallet_value (encrypted_wallet_key, 0));
	}
	else
	{
		result = true;
	}
	return result;
}

void nano::wallet_store::derive_key (nano::raw_key & result, nano::store::transaction const & txn_wallet, std::string const & password_text) const
{
	kdf.phs (result, password_text, salt (txn_wallet));
}

auto nano::wallet_store::begin (nano::store::transaction const & txn_wallet) const -> iterator
{
	return repository.begin (txn_wallet);
}

auto nano::wallet_store::begin (nano::store::transaction const & txn_wallet, nano::account const & key) const -> iterator
{
	return repository.begin (txn_wallet, key);
}

auto nano::wallet_store::end (nano::store::transaction const & txn_wallet) const -> iterator
{
	return repository.end (txn_wallet);
}

auto nano::wallet_store::find (nano::store::transaction const & txn_wallet, nano::account const & key) const -> iterator
{
	return repository.find (txn_wallet, key);
}

void nano::wallet_store::destroy (nano::store::write_transaction const & txn_wallet)
{
	repository.destroy (txn_wallet);
}
