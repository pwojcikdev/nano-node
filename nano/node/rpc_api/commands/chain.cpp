#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>

using namespace nano::rpc_api;

class chain_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "chain";
	}

	command_result handle (request_context const & ctx) override
	{
		return do_chain (ctx, false);
	}

protected:
	command_result do_chain (request_context const & ctx, bool default_reverse)
	{
		auto & p = ctx.params ();

		auto hash_r = params::required_hash (p, "block");
		if (!hash_r)
			return command_result::error (hash_r.error_message ());
		auto hash = *hash_r;

		auto count_r = params::required_count (p);
		if (!count_r)
			return command_result::error (count_r.error_message ());
		auto count = *count_r;

		auto offset = params::optional_uint64 (p, "offset", 0);
		bool reverse = params::optional_bool (p, "reverse", false);

		// XOR: if called as "successors" (default_reverse=true), reverse the logic
		bool successors = default_reverse != reverse;

		boost::json::array blocks;
		auto transaction = ctx.node.ledger.tx_begin_read ();
		while (!hash.is_zero () && blocks.size () < count)
		{
			auto block_l = ctx.node.ledger.any.block_get (transaction, hash);
			if (block_l != nullptr)
			{
				if (offset > 0)
				{
					--offset;
				}
				else
				{
					blocks.emplace_back (hash.to_string ());
				}
				hash = successors ? ctx.node.ledger.any.block_successor (transaction, hash).value_or (0) : block_l->previous ();
			}
			else
			{
				hash.clear ();
			}
		}
		boost::json::object result;
		result["blocks"] = std::move (blocks);
		return command_result::ok (std::move (result));
	}
};

class successors_command final : public chain_command
{
public:
	std::string_view name () const override
	{
		return "successors";
	}

	command_result handle (request_context const & ctx) override
	{
		return do_chain (ctx, true);
	}
};

REGISTER_RPC_COMMAND (chain_command);
REGISTER_RPC_COMMAND (successors_command);
