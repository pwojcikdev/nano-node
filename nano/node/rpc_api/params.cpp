#include <nano/node/node.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/wallet.hpp>

#include <boost/json/value.hpp>

namespace nano::rpc_api::params
{
namespace
{
	std::string value_as_string (boost::json::value const & v)
	{
		if (v.is_string ())
			return std::string (v.as_string ());
		if (v.is_int64 ())
			return std::to_string (v.as_int64 ());
		if (v.is_uint64 ())
			return std::to_string (v.as_uint64 ());
		if (v.is_bool ())
			return v.as_bool () ? "true" : "false";
		return {};
	}

	bool value_as_bool (boost::json::value const & v, bool default_value)
	{
		if (v.is_bool ())
			return v.as_bool ();
		if (v.is_string ())
		{
			auto s = v.as_string ();
			if (s == "true" || s == "1")
				return true;
			if (s == "false" || s == "0")
				return false;
		}
		if (v.is_int64 ())
			return v.as_int64 () != 0;
		if (v.is_uint64 ())
			return v.as_uint64 () != 0;
		return default_value;
	}

	std::optional<uint64_t> value_as_uint64 (boost::json::value const & v)
	{
		if (v.is_uint64 ())
			return v.as_uint64 ();
		if (v.is_int64 () && v.as_int64 () >= 0)
			return static_cast<uint64_t> (v.as_int64 ());
		if (v.is_string ())
		{
			try
			{
				auto str = std::string (v.as_string ());
				std::size_t end;
				auto val = std::stoull (str, &end);
				if (end == str.size ())
					return val;
			}
			catch (...)
			{
			}
		}
		return std::nullopt;
	}
}

result<std::string> required_string (boost::json::object const & obj, std::string_view key)
{
	auto it = obj.find (key);
	if (it == obj.end ())
		return result<std::string>::fail (std::string ("Missing required parameter: ") + std::string (key));
	auto str = value_as_string (it->value ());
	if (str.empty () && !it->value ().is_string ())
		return result<std::string>::fail (std::string ("Invalid value for parameter: ") + std::string (key));
	return str;
}

result<nano::account> required_account (boost::json::object const & obj, std::string_view key)
{
	auto str = required_string (obj, key);
	if (!str)
		return result<nano::account>::fail (str.error_message ());
	nano::account account;
	if (account.decode_account (*str))
		return result<nano::account>::fail ("Bad account number");
	return account;
}

result<nano::block_hash> required_hash (boost::json::object const & obj, std::string_view key)
{
	auto str = required_string (obj, key);
	if (!str)
		return result<nano::block_hash>::fail (str.error_message ());
	nano::block_hash hash;
	if (hash.decode_hex (*str))
		return result<nano::block_hash>::fail ("Bad hash");
	return hash;
}

result<uint64_t> required_count (boost::json::object const & obj, std::string_view key)
{
	auto it = obj.find (key);
	if (it == obj.end ())
		return result<uint64_t>::fail (std::string ("Missing required parameter: ") + std::string (key));
	auto val = value_as_uint64 (it->value ());
	if (!val || *val == 0)
		return result<uint64_t>::fail ("Invalid count");
	return *val;
}

result<nano::amount> required_amount (boost::json::object const & obj, std::string_view key)
{
	auto str = required_string (obj, key);
	if (!str)
		return result<nano::amount>::fail (str.error_message ());
	nano::amount amount;
	if (amount.decode_dec (*str))
		return result<nano::amount>::fail ("Bad amount");
	return amount;
}

result<std::shared_ptr<nano::wallet>> required_wallet (nano::node & node, boost::json::object const & obj, std::string_view key)
{
	auto str = required_string (obj, key);
	if (!str)
		return result<std::shared_ptr<nano::wallet>>::fail (str.error_message ());
	nano::wallet_id wallet_id;
	if (wallet_id.decode_hex (*str))
		return result<std::shared_ptr<nano::wallet>>::fail ("Bad wallet number");
	auto existing = node.wallets.open (wallet_id);
	if (!existing)
		return result<std::shared_ptr<nano::wallet>>::fail ("Wallet not found");
	return existing;
}

bool optional_bool (boost::json::object const & obj, std::string_view key, bool default_value)
{
	auto it = obj.find (key);
	if (it == obj.end ())
		return default_value;
	return value_as_bool (it->value (), default_value);
}

std::string optional_string (boost::json::object const & obj, std::string_view key, std::string_view default_value)
{
	auto it = obj.find (key);
	if (it == obj.end ())
		return std::string (default_value);
	return value_as_string (it->value ());
}

uint64_t optional_uint64 (boost::json::object const & obj, std::string_view key, uint64_t default_value)
{
	auto it = obj.find (key);
	if (it == obj.end ())
		return default_value;
	return value_as_uint64 (it->value ()).value_or (default_value);
}

nano::amount optional_threshold (boost::json::object const & obj, std::string_view key)
{
	nano::amount result (0);
	auto it = obj.find (key);
	if (it == obj.end ())
		return result;
	auto str = value_as_string (it->value ());
	if (!str.empty ())
		result.decode_dec (str);
	return result;
}

uint64_t optional_count (boost::json::object const & obj, uint64_t default_value)
{
	auto it = obj.find ("count");
	if (it == obj.end ())
		return default_value;
	return value_as_uint64 (it->value ()).value_or (default_value);
}

result<std::vector<std::string>> required_string_array (boost::json::object const & obj, std::string_view key)
{
	auto it = obj.find (key);
	if (it == obj.end ())
		return result<std::vector<std::string>>::fail (std::string ("Missing required parameter: ") + std::string (key));
	if (!it->value ().is_array ())
		return result<std::vector<std::string>>::fail (std::string ("Expected array for parameter: ") + std::string (key));
	std::vector<std::string> values;
	for (auto const & elem : it->value ().as_array ())
		values.push_back (value_as_string (elem));
	return values;
}
}
