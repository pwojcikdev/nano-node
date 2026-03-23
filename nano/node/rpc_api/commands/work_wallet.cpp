#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/node/wallet.hpp>

using namespace nano::rpc_api;

// work_get: Get cached work value for an account in a wallet (sync)
class work_get_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "work_get";
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

		auto account = params::required_account (ctx.params ());
		if (!account)
			return command_result::error (account.error_message ());

		if (!(*wallet)->exists (*account))
		{
			return command_result::error ("Account not found in wallet");
		}

		uint64_t work (0);
		auto error_work = (*wallet)->get_work (*account, work);
		(void)error_work;
		return command_result::ok ({ { "work", nano::to_string_hex (work) } });
	}
};
REGISTER_RPC_COMMAND (work_get_cmd);

// work_set: Set cached work value for an account in a wallet (async)
class work_set_cmd final : public command
{
	std::string_view name () const override
	{
		return "work_set";
	}
	bool requires_control () const override
	{
		return true;
	}

	void execute (request_context & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
		{
			ctx.respond_error (wallet.error_message ());
			return;
		}

		auto account = params::required_account (ctx.params ());
		if (!account)
		{
			ctx.respond_error (account.error_message ());
			return;
		}

		auto work_str = params::required_string (ctx.params (), "work");
		if (!work_str)
		{
			ctx.respond_error (work_str.error_message ());
			return;
		}

		uint64_t work;
		if (nano::from_string_hex (*work_str, work))
		{
			ctx.respond_error ("Bad work");
			return;
		}

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto wallet_ptr = *wallet;
		auto account_val = *account;

		ctx.node.workers.post ([wallet_ptr, account_val, work, respond, respond_error] () {
			try
			{
				if (!wallet_ptr->exists (account_val))
				{
					respond_error ("Account not found in wallet");
					return;
				}
				wallet_ptr->set_work (account_val, work);
				respond (boost::json::object{ { "success", "" } });
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (work_set_cmd);

// work_generate: Generate work for a hash (async with callbacks)
// STUB - Very complex handler with distributed work, callbacks, block validation,
// difficulty/multiplier calculations, and work peer support.
// Left for incremental migration.
// REGISTER_RPC_COMMAND (work_generate_cmd);
