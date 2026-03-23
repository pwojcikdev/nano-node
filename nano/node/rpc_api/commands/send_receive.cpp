#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>

using namespace nano::rpc_api;

// send: Send nano from a wallet account (async with callback)
// STUB - Complex handler using wallet.send_async with callback pattern.
// Requires work validation, balance checking, and send_id support.
// Left for incremental migration.
// REGISTER_RPC_COMMAND (send_cmd);

// receive: Receive a pending block into a wallet account (async with callback)
// STUB - Complex handler using wallet.receive_async with callback pattern.
// Requires work validation, pending block verification, and epoch handling.
// Left for incremental migration.
// REGISTER_RPC_COMMAND (receive_cmd);

// search_receivable: Search for receivable blocks in a wallet
class search_receivable_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "search_receivable";
	}
	bool requires_control () const override
	{
		return true;
	}

	command_result handle (request_context const & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		auto error = (*wallet)->search_receivable ();
		return command_result::ok ({ { "started", !error ? "1" : "0" } });
	}
};
REGISTER_RPC_COMMAND (search_receivable_cmd);

// search_receivable_all: Search all wallets for receivable blocks
class search_receivable_all_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "search_receivable_all";
	}
	bool requires_control () const override
	{
		return true;
	}

	command_result handle (request_context const & ctx) override
	{
		ctx.node.wallets.search_receivable_all ();
		return command_result::ok ({ { "success", "" } });
	}
};
REGISTER_RPC_COMMAND (search_receivable_all_cmd);

// search_pending: Deprecated alias for search_receivable
class search_pending_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "search_pending";
	}
	bool requires_control () const override
	{
		return true;
	}

	command_result handle (request_context const & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		auto error = (*wallet)->search_receivable ();
		boost::json::object result;
		result["deprecated"] = "1";
		result["started"] = !error ? "1" : "0";
		return command_result::ok (result);
	}
};
REGISTER_RPC_COMMAND (search_pending_cmd);

// search_pending_all: Deprecated alias for search_receivable_all
class search_pending_all_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "search_pending_all";
	}
	bool requires_control () const override
	{
		return true;
	}

	command_result handle (request_context const & ctx) override
	{
		ctx.node.wallets.search_receivable_all ();
		boost::json::object result;
		result["deprecated"] = "1";
		result["success"] = "";
		return command_result::ok (result);
	}
};
REGISTER_RPC_COMMAND (search_pending_all_cmd);
