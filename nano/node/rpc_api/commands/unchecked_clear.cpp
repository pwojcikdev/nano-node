#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>

using namespace nano::rpc_api;

// unchecked_clear: Clear all unchecked blocks (async)
class unchecked_clear_cmd final : public command
{
	std::string_view name () const override
	{
		return "unchecked_clear";
	}
	bool requires_control () const override
	{
		return true;
	}

	void execute (request_context & ctx) override
	{
		auto respond = ctx.respond;
		auto respond_error = ctx.respond_error;
		auto & node = ctx.node;

		node.workers.post ([&node, respond, respond_error] () {
			try
			{
				node.unchecked.clear ();
				respond (boost::json::object{ { "success", "" } });
			}
			catch (...)
			{
				respond_error ("Internal server error in RPC");
			}
		});
	}
};
REGISTER_RPC_COMMAND (unchecked_clear_cmd);
