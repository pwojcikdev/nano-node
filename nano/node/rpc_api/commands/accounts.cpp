#include <nano/lib/numbers.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>

namespace nano::rpc_api
{
// accounts_balances
class accounts_balances_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "accounts_balances";
	}

	command_result handle (request_context const & ctx) override
	{
		auto accounts_arr = params::required_string_array (ctx.params (), "accounts");
		if (!accounts_arr)
			return command_result::error (accounts_arr.error_message ());

		bool include_only_confirmed = params::optional_bool (ctx.params (), "include_only_confirmed", true);

		boost::json::object balances;
		boost::json::object errors;
		auto transaction = ctx.node.store.tx_begin_read ();

		for (auto const & account_str : *accounts_arr)
		{
			nano::account account;
			if (account.decode_account (account_str))
			{
				errors[account_str] = "Bad account number";
				continue;
			}

			auto balance = ctx.node.balance_pending (account, include_only_confirmed);
			boost::json::object entry;
			entry["balance"] = balance.first.convert_to<std::string> ();
			entry["pending"] = balance.second.convert_to<std::string> ();
			entry["receivable"] = balance.second.convert_to<std::string> ();
			balances[account_str] = std::move (entry);
		}

		boost::json::object response;
		if (!balances.empty ())
		{
			response["balances"] = std::move (balances);
		}
		if (!errors.empty ())
		{
			response["errors"] = std::move (errors);
		}
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (accounts_balances_command);

// accounts_representatives
class accounts_representatives_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "accounts_representatives";
	}

	command_result handle (request_context const & ctx) override
	{
		auto accounts_arr = params::required_string_array (ctx.params (), "accounts");
		if (!accounts_arr)
			return command_result::error (accounts_arr.error_message ());

		boost::json::object representatives;
		boost::json::object errors;
		auto transaction = ctx.node.ledger.tx_begin_read ();

		for (auto const & account_str : *accounts_arr)
		{
			nano::account account;
			if (account.decode_account (account_str))
			{
				errors[account_str] = "Bad account number";
				continue;
			}

			auto info = ctx.node.ledger.any.account_get (transaction, account);
			if (info)
			{
				representatives[account_str] = info->representative.to_account ();
			}
			else
			{
				errors[account_str] = "Account not found";
			}
		}

		boost::json::object response;
		if (!representatives.empty ())
		{
			response["representatives"] = std::move (representatives);
		}
		if (!errors.empty ())
		{
			response["errors"] = std::move (errors);
		}
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (accounts_representatives_command);

// accounts_frontiers
class accounts_frontiers_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "accounts_frontiers";
	}

	command_result handle (request_context const & ctx) override
	{
		auto accounts_arr = params::required_string_array (ctx.params (), "accounts");
		if (!accounts_arr)
			return command_result::error (accounts_arr.error_message ());

		boost::json::object frontiers;
		boost::json::object errors;
		auto transaction = ctx.node.ledger.tx_begin_read ();

		for (auto const & account_str : *accounts_arr)
		{
			nano::account account;
			if (account.decode_account (account_str))
			{
				errors[account_str] = "Bad account number";
				continue;
			}

			auto latest = ctx.node.ledger.any.account_head (transaction, account);
			if (!latest.is_zero ())
			{
				frontiers[account.to_account ()] = latest.to_string ();
			}
			else
			{
				errors[account_str] = "Account not found";
			}
		}

		boost::json::object response;
		if (!frontiers.empty ())
		{
			response["frontiers"] = std::move (frontiers);
		}
		if (!errors.empty ())
		{
			response["errors"] = std::move (errors);
		}
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (accounts_frontiers_command);
}
