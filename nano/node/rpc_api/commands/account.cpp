#include <nano/lib/numbers.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>

namespace nano::rpc_api
{
// account_balance
class account_balance_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "account_balance";
	}

	command_result handle (request_context const & ctx) override
	{
		auto account = params::required_account (ctx.params ());
		if (!account)
			return command_result::error (account.error_message ());

		bool include_only_confirmed = params::optional_bool (ctx.params (), "include_only_confirmed", true);
		auto balance = ctx.node.balance_pending (*account, include_only_confirmed);

		boost::json::object response;
		response["balance"] = balance.first.convert_to<std::string> ();
		response["pending"] = balance.second.convert_to<std::string> ();
		response["receivable"] = balance.second.convert_to<std::string> ();
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (account_balance_command);

// account_block_count
class account_block_count_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "account_block_count";
	}

	command_result handle (request_context const & ctx) override
	{
		auto account = params::required_account (ctx.params ());
		if (!account)
			return command_result::error (account.error_message ());

		auto transaction = ctx.node.ledger.tx_begin_read ();
		auto info = ctx.node.ledger.any.account_get (transaction, *account);
		if (!info)
			return command_result::error ("Account not found");

		boost::json::object response;
		response["block_count"] = std::to_string (info->block_count);
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (account_block_count_command);

// account_count (alias: frontier_count)
class account_count_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "account_count";
	}

	command_result handle (request_context const & ctx) override
	{
		auto size = ctx.node.ledger.account_count ();
		boost::json::object response;
		response["count"] = std::to_string (size);
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (account_count_command);

// frontier_count (alias for account_count)
class frontier_count_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "frontier_count";
	}

	command_result handle (request_context const & ctx) override
	{
		auto size = ctx.node.ledger.account_count ();
		boost::json::object response;
		response["count"] = std::to_string (size);
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (frontier_count_command);

// account_get
class account_get_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "account_get";
	}

	command_result handle (request_context const & ctx) override
	{
		auto key_str = params::required_string (ctx.params (), "key");
		if (!key_str)
			return command_result::error (key_str.error_message ());

		nano::public_key pub;
		if (pub.decode_hex (*key_str))
			return command_result::error ("Bad public key");

		boost::json::object response;
		response["account"] = pub.to_account ();
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (account_get_command);

// account_key
class account_key_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "account_key";
	}

	command_result handle (request_context const & ctx) override
	{
		auto account = params::required_account (ctx.params ());
		if (!account)
			return command_result::error (account.error_message ());

		boost::json::object response;
		response["key"] = account->to_string ();
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (account_key_command);

// account_representative
class account_representative_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "account_representative";
	}

	command_result handle (request_context const & ctx) override
	{
		auto account = params::required_account (ctx.params ());
		if (!account)
			return command_result::error (account.error_message ());

		auto transaction = ctx.node.ledger.tx_begin_read ();
		auto info = ctx.node.ledger.any.account_get (transaction, *account);
		if (!info)
			return command_result::error ("Account not found");

		boost::json::object response;
		response["representative"] = info->representative.to_account ();
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (account_representative_command);

// account_weight
class account_weight_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "account_weight";
	}

	command_result handle (request_context const & ctx) override
	{
		auto account = params::required_account (ctx.params ());
		if (!account)
			return command_result::error (account.error_message ());

		auto balance = ctx.node.weight (*account);
		boost::json::object response;
		response["weight"] = balance.convert_to<std::string> ();
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (account_weight_command);

// validate_account_number
class validate_account_number_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "validate_account_number";
	}

	command_result handle (request_context const & ctx) override
	{
		auto account = params::required_account (ctx.params ());
		boost::json::object response;
		response["valid"] = account ? "1" : "0";
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (validate_account_number_command);
}
