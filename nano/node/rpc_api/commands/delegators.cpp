#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/store/ledger/account.hpp>

using namespace nano::rpc_api;

class delegators_command final : public sync_command
{
	std::string_view name () const override
	{
		return "delegators";
	}
	command_result handle (request_context const & ctx) override
	{
		auto representative = params::required_account (ctx.params ());
		if (!representative)
			return command_result::error (representative.error_message ());
		auto count = params::optional_count (ctx.params (), 1024);
		auto threshold = params::optional_threshold (ctx.params ());
		auto start_text = params::optional_string (ctx.params (), "start");

		nano::account start_account{};
		if (!start_text.empty ())
		{
			if (start_account.decode_account (start_text))
				return command_result::error ("Bad account number");
		}

		auto transaction = ctx.node.ledger.tx_begin_read ();
		boost::json::object delegators;
		for (auto i = ctx.node.store.account.begin (transaction, start_account.number () == std::numeric_limits<nano::uint256_t>::max () ? start_account : nano::account (start_account.number () + 1)),
				  n = ctx.node.store.account.end (transaction);
			 i != n && delegators.size () < count; ++i)
		{
			nano::account_info const & info = i->second;
			if (info.representative == *representative && info.balance.number () >= threshold.number ())
			{
				nano::account const & delegator = i->first;
				delegators[delegator.to_account ()] = nano::uint128_union (info.balance).to_string_dec ();
			}
		}
		return command_result::ok ({ { "delegators", delegators } });
	}
};
REGISTER_RPC_COMMAND (delegators_command);

class delegators_count_command final : public sync_command
{
	std::string_view name () const override
	{
		return "delegators_count";
	}
	command_result handle (request_context const & ctx) override
	{
		auto account = params::required_account (ctx.params ());
		if (!account)
			return command_result::error (account.error_message ());
		uint64_t count = 0;
		auto transaction = ctx.node.ledger.tx_begin_read ();
		for (auto i = ctx.node.store.account.begin (transaction), n = ctx.node.store.account.end (transaction); i != n; ++i)
		{
			if (i->second.representative == *account)
				++count;
		}
		return command_result::ok ({ { "count", std::to_string (count) } });
	}
};
REGISTER_RPC_COMMAND (delegators_count_command);
