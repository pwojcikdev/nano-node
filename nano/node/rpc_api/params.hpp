#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/node/fwd.hpp>

#include <boost/json/object.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nano
{
class wallet;
}

namespace nano::rpc_api::params
{
// Result type for parameter extraction: holds either a value or an error message
template <typename T>
class result
{
public:
	result (T value) :
		value_ (std::move (value))
	{
	}
	static result fail (std::string msg)
	{
		return result (std::nullopt, std::move (msg));
	}

	explicit operator bool () const
	{
		return value_.has_value ();
	}
	bool has_value () const
	{
		return value_.has_value ();
	}
	T & operator* ()
	{
		return *value_;
	}
	T const & operator* () const
	{
		return *value_;
	}
	T * operator->()
	{
		return &*value_;
	}
	T const * operator->() const
	{
		return &*value_;
	}
	std::string const & error_message () const
	{
		return error_;
	}

private:
	result (std::optional<T> v, std::string err) :
		value_ (std::move (v)),
		error_ (std::move (err))
	{
	}
	std::optional<T> value_;
	std::string error_;
};

// --- Required parameter extraction ---

result<std::string> required_string (boost::json::object const & obj, std::string_view key);
result<nano::account> required_account (boost::json::object const & obj, std::string_view key = "account");
result<nano::block_hash> required_hash (boost::json::object const & obj, std::string_view key = "hash");
result<uint64_t> required_count (boost::json::object const & obj, std::string_view key = "count");
result<nano::amount> required_amount (boost::json::object const & obj, std::string_view key = "amount");
result<std::shared_ptr<nano::wallet>> required_wallet (nano::node & node, boost::json::object const & obj, std::string_view key = "wallet");

// --- Optional parameter extraction ---

bool optional_bool (boost::json::object const & obj, std::string_view key, bool default_value = false);
std::string optional_string (boost::json::object const & obj, std::string_view key, std::string_view default_value = "");
uint64_t optional_uint64 (boost::json::object const & obj, std::string_view key, uint64_t default_value = 0);
nano::amount optional_threshold (boost::json::object const & obj, std::string_view key = "threshold");
uint64_t optional_count (boost::json::object const & obj, uint64_t default_value = std::numeric_limits<uint64_t>::max ());

// --- Array parameter extraction ---

result<std::vector<std::string>> required_string_array (boost::json::object const & obj, std::string_view key);
}
