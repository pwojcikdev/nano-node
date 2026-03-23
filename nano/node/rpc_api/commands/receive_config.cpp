#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>

using namespace nano::rpc_api;

class receive_minimum_command final : public sync_command
{
	std::string_view name () const override
	{
		return "receive_minimum";
	}
	bool requires_control () const override
	{
		return true;
	}
	command_result handle (request_context const & ctx) override
	{
		return command_result::ok ({ { "amount", ctx.node.config.receive_minimum.to_string_dec () } });
	}
};
REGISTER_RPC_COMMAND (receive_minimum_command);

class receive_minimum_set_command final : public sync_command
{
	std::string_view name () const override
	{
		return "receive_minimum_set";
	}
	bool requires_control () const override
	{
		return true;
	}
	command_result handle (request_context const & ctx) override
	{
		auto amount = params::required_amount (ctx.params ());
		if (!amount)
			return command_result::error (amount.error_message ());
		ctx.node.config.receive_minimum = *amount;
		return command_result::ok ({ { "success", "" } });
	}
};
REGISTER_RPC_COMMAND (receive_minimum_set_command);

class populate_backlog_command final : public sync_command
{
	std::string_view name () const override
	{
		return "populate_backlog";
	}
	bool requires_control () const override
	{
		return true;
	}
	command_result handle (request_context const & ctx) override
	{
		ctx.node.backlog_scan.trigger ();
		return command_result::ok ({ { "success", "" } });
	}
};
REGISTER_RPC_COMMAND (populate_backlog_command);
