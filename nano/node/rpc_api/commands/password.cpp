#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/node/wallet.hpp>

using namespace nano::rpc_api;

// password_change: Change wallet password (async)
class password_change_cmd final : public command
{
	std::string_view name () const override
	{
		return "password_change";
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

		auto password_str = params::required_string (ctx.params (), "password");
		if (!password_str)
		{
			ctx.respond_error (password_str.error_message ());
			return;
		}

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto wallet_ptr = *wallet;
		auto password = *password_str;
		auto & node = ctx.node;

		node.workers.post ([wallet_ptr, password, respond, respond_error, &node] () {
			try
			{
				if (wallet_ptr->is_locked ())
				{
					respond_error ("Wallet locked");
					return;
				}
				bool error = wallet_ptr->rekey (password);
				if (!error)
				{
					node.logger.warn (nano::log::type::rpc, "Wallet password changed");
				}
				respond (boost::json::object{ { "changed", error ? "0" : "1" } });
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (password_change_cmd);

// password_enter (alias: wallet_unlock): Unlock a wallet (async)
class password_enter_cmd final : public command
{
	std::string_view name () const override
	{
		return "password_enter";
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

		auto password_str = params::required_string (ctx.params (), "password");
		if (!password_str)
		{
			ctx.respond_error (password_str.error_message ());
			return;
		}

		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto wallet_ptr = *wallet;
		auto password = *password_str;

		ctx.node.workers.post ([wallet_ptr, password, respond, respond_error] () {
			try
			{
				auto error = wallet_ptr->enter_password (password);
				respond (boost::json::object{ { "valid", error ? "0" : "1" } });
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (password_enter_cmd);

// wallet_unlock: alias for password_enter
static struct
{
	struct reg
	{
		reg ()
		{
			registry::instance ().register_alias ("wallet_unlock", "password_enter");
		}
	} r;
} _wallet_unlock_alias;

// password_valid: Check wallet password validity (sync)
class password_valid_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "password_valid";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		auto valid = !(*wallet)->is_locked ();
		return command_result::ok ({ { "valid", valid ? "1" : "0" } });
	}
};
REGISTER_RPC_COMMAND (password_valid_cmd);

// wallet_locked: Check if wallet is locked (variant of password_valid)
class wallet_locked_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "wallet_locked";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		auto wallet = params::required_wallet (ctx.node, ctx.params ());
		if (!wallet)
			return command_result::error (wallet.error_message ());

		auto valid = !(*wallet)->is_locked ();
		return command_result::ok ({ { "locked", valid ? "0" : "1" } });
	}
};
REGISTER_RPC_COMMAND (wallet_locked_cmd);
