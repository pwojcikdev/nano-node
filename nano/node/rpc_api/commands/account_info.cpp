#include <nano/lib/numbers.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/ledger/confirmation_height.hpp>

namespace
{
char const * epoch_as_string (nano::epoch epoch)
{
	switch (epoch)
	{
		case nano::epoch::epoch_2:
			return "2";
		case nano::epoch::epoch_1:
			return "1";
		default:
			return "0";
	}
}
}

namespace nano::rpc_api
{
class account_info_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "account_info";
	}

	command_result handle (request_context const & ctx) override
	{
		auto account = params::required_account (ctx.params ());
		if (!account)
			return command_result::error (account.error_message ());

		bool const representative = params::optional_bool (ctx.params (), "representative", false);
		bool const weight = params::optional_bool (ctx.params (), "weight", false);
		bool const pending = params::optional_bool (ctx.params (), "pending", false);
		bool const receivable = params::optional_bool (ctx.params (), "receivable", pending);
		bool const include_confirmed = params::optional_bool (ctx.params (), "include_confirmed", false);

		auto transaction = ctx.node.ledger.tx_begin_read ();
		auto info = ctx.node.ledger.any.account_get (transaction, *account);
		if (!info)
			return command_result::error ("Account not found");

		nano::confirmation_height_info confirmation_height_info;
		ctx.node.store.confirmation_height.get (transaction, *account, confirmation_height_info);

		boost::json::object response;

		response["frontier"] = info->head.to_string ();
		response["open_block"] = info->open_block.to_string ();
		response["representative_block"] = ctx.node.ledger.representative_block (transaction, info->head).to_string ();

		nano::amount balance_l (info->balance);
		std::string balance = balance_l.to_string_dec ();
		response["balance"] = balance;

		if (include_confirmed)
		{
			nano::amount confirmed_balance_l;
			if (info->block_count != confirmation_height_info.height)
			{
				confirmed_balance_l = ctx.node.ledger.any.block_balance (transaction, confirmation_height_info.frontier).value_or (0);
			}
			else
			{
				confirmed_balance_l = balance_l;
			}
			std::string confirmed_balance = confirmed_balance_l.to_string_dec ();
			response["confirmed_balance"] = confirmed_balance;
		}

		response["modified_timestamp"] = std::to_string (info->modified);
		response["block_count"] = std::to_string (info->block_count);
		response["account_version"] = epoch_as_string (info->epoch ());

		auto confirmed_frontier = confirmation_height_info.frontier.to_string ();
		if (include_confirmed)
		{
			response["confirmed_height"] = std::to_string (confirmation_height_info.height);
			response["confirmed_frontier"] = confirmed_frontier;
		}
		else
		{
			// For backwards compatibility purposes
			response["confirmation_height"] = std::to_string (confirmation_height_info.height);
			response["confirmation_height_frontier"] = confirmed_frontier;
		}

		std::shared_ptr<nano::block> confirmed_frontier_block;
		if (include_confirmed && confirmation_height_info.height > 0)
		{
			confirmed_frontier_block = ctx.node.ledger.any.block_get (transaction, confirmation_height_info.frontier);
		}

		if (representative)
		{
			response["representative"] = info->representative.to_account ();
			if (include_confirmed)
			{
				nano::account confirmed_representative{};
				if (confirmed_frontier_block)
				{
					confirmed_representative = confirmed_frontier_block->representative_field ().value_or (0);
					if (confirmed_representative.is_zero ())
					{
						confirmed_representative = ctx.node.ledger.any.block_get (transaction, ctx.node.ledger.representative_block (transaction, confirmation_height_info.frontier))->representative_field ().value ();
					}
				}
				response["confirmed_representative"] = confirmed_representative.to_account ();
			}
		}

		if (weight)
		{
			auto account_weight = ctx.node.ledger.weight_exact (transaction, *account);
			response["weight"] = account_weight.convert_to<std::string> ();
		}

		if (receivable)
		{
			auto account_receivable = ctx.node.ledger.account_receivable (transaction, *account);
			response["pending"] = account_receivable.convert_to<std::string> ();
			response["receivable"] = account_receivable.convert_to<std::string> ();

			if (include_confirmed)
			{
				auto confirmed_receivable = ctx.node.ledger.account_receivable (transaction, *account, true);
				response["confirmed_pending"] = confirmed_receivable.convert_to<std::string> ();
				response["confirmed_receivable"] = confirmed_receivable.convert_to<std::string> ();
			}
		}

		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (account_info_command);
}
