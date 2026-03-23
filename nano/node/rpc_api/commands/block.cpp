#include <nano/lib/block_sideband.hpp>
#include <nano/lib/block_type.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/cementing_set.hpp>
#include <nano/node/election_status.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_cemented.hpp>
#include <nano/store/ledger/successor.hpp>

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace nano::rpc_api
{
// block_info
class block_info_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "block_info";
	}

	command_result handle (request_context const & ctx) override
	{
		auto hash = params::required_hash (ctx.params ());
		if (!hash)
			return command_result::error (hash.error_message ());

		auto transaction = ctx.node.ledger.tx_begin_read ();
		auto block = ctx.node.ledger.any.block_get (transaction, *hash);
		if (!block)
			return command_result::error ("Block not found");

		boost::json::object response;
		auto account = block->account ();
		response["block_account"] = account.to_account ();

		bool include_linked_account = params::optional_bool (ctx.params (), "include_linked_account", false);
		if (include_linked_account)
		{
			auto linked_account = ctx.node.ledger.linked_account (transaction, *block);
			if (linked_account.has_value ())
			{
				response["linked_account"] = linked_account.value ().to_account ();
			}
			else
			{
				response["linked_account"] = "0";
			}
		}

		auto amount = ctx.node.ledger.any.block_amount (transaction, *hash);
		if (amount)
		{
			response["amount"] = amount.value ().number ().convert_to<std::string> ();
		}
		auto balance = block->balance ();
		response["balance"] = balance.number ().convert_to<std::string> ();
		response["height"] = std::to_string (block->sideband ().height);
		response["local_timestamp"] = std::to_string (block->sideband ().timestamp);
		response["successor"] = ctx.node.store.successor.get (transaction, *hash).value_or (nano::block_hash{ 0 }).to_string ();
		auto confirmed = ctx.node.ledger.cemented.block_exists_or_pruned (transaction, *hash);
		response["confirmed"] = confirmed ? "true" : "false";

		bool json_block = params::optional_bool (ctx.params (), "json_block", false);
		if (json_block)
		{
			boost::property_tree::ptree block_node;
			block->serialize_json (block_node);
			std::stringstream ostream;
			boost::property_tree::write_json (ostream, block_node);
			auto parsed = boost::json::parse (ostream.str ());
			response["contents"] = parsed;
		}
		else
		{
			std::string contents;
			block->serialize_json (contents);
			response["contents"] = contents;
		}

		if (block->type () == nano::block_type::state)
		{
			auto subtype = nano::state_subtype (block->sideband ().details);
			response["subtype"] = subtype;
		}

		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (block_info_command);

// block_account
class block_account_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "block_account";
	}

	command_result handle (request_context const & ctx) override
	{
		auto hash = params::required_hash (ctx.params ());
		if (!hash)
			return command_result::error (hash.error_message ());

		auto transaction = ctx.node.ledger.tx_begin_read ();
		auto block = ctx.node.ledger.any.block_get (transaction, *hash);
		if (!block)
			return command_result::error ("Block not found");

		boost::json::object response;
		response["account"] = block->account ().to_account ();
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (block_account_command);

// block_count
class block_count_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "block_count";
	}

	command_result handle (request_context const & ctx) override
	{
		boost::json::object response;
		response["count"] = std::to_string (ctx.node.ledger.block_count ());
		response["unchecked"] = std::to_string (ctx.node.unchecked.count ());
		response["cemented"] = std::to_string (ctx.node.ledger.cemented_count ());
		if (ctx.node.flags.enable_pruning)
		{
			response["full"] = std::to_string (ctx.node.ledger.block_count () - ctx.node.ledger.pruned_count ());
			response["pruned"] = std::to_string (ctx.node.ledger.pruned_count ());
		}
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (block_count_command);

// block_hash
class block_hash_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "block_hash";
	}

	command_result handle (request_context const & ctx) override
	{
		bool json_block = params::optional_bool (ctx.params (), "json_block", false);

		boost::property_tree::ptree block_ptree;
		if (json_block)
		{
			// "block" is a JSON object in the request
			auto it = ctx.params ().find ("block");
			if (it == ctx.params ().end ())
				return command_result::error ("Missing required parameter: block");
			// Serialize the boost::json value to string, then parse as property_tree
			auto block_json_str = boost::json::serialize (it->value ());
			std::stringstream ss (block_json_str);
			try
			{
				boost::property_tree::read_json (ss, block_ptree);
			}
			catch (...)
			{
				return command_result::error ("Invalid block");
			}
		}
		else
		{
			// "block" is a JSON-encoded string
			auto block_str = params::required_string (ctx.params (), "block");
			if (!block_str)
				return command_result::error (block_str.error_message ());
			std::stringstream ss (*block_str);
			try
			{
				boost::property_tree::read_json (ss, block_ptree);
			}
			catch (...)
			{
				return command_result::error ("Invalid block");
			}
		}

		auto block = nano::deserialize_block_json (block_ptree);
		if (!block)
			return command_result::error ("Invalid block");

		boost::json::object response;
		response["hash"] = block->hash ().to_string ();
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (block_hash_command);

// block_confirm
class block_confirm_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "block_confirm";
	}

	command_result handle (request_context const & ctx) override
	{
		auto hash = params::required_hash (ctx.params ());
		if (!hash)
			return command_result::error (hash.error_message ());

		auto transaction = ctx.node.ledger.tx_begin_read ();
		auto block_l = ctx.node.ledger.any.block_get (transaction, *hash);
		if (!block_l)
			return command_result::error ("Block not found");

		if (!ctx.node.ledger.cemented.block_exists_or_pruned (transaction, *hash))
		{
			// Start new confirmation for unconfirmed (or not being confirmed) block
			if (!ctx.node.cementing_set.contains (*hash))
			{
				ctx.node.start_election (std::move (block_l));
			}
		}
		else
		{
			// Add record in confirmation history for confirmed block
			nano::election_status status{ block_l, nano::election_status_type::active_confirmation_height };
			ctx.node.active.recently_cemented.put (status);
			// Trigger callback for confirmed block
			auto account = block_l->account ();
			auto amount = ctx.node.ledger.any.block_amount (transaction, *hash);
			bool is_state_send = false;
			bool is_state_epoch = false;
			if (amount)
			{
				if (auto state = dynamic_cast<nano::state_block *> (block_l.get ()))
				{
					is_state_send = state->is_send ();
					is_state_epoch = amount.value () == 0 && ctx.node.ledger.is_epoch_link (state->link_field ().value ());
				}
			}
			ctx.node.observers.blocks.notify (status, {}, account, amount ? amount.value ().number () : 0, is_state_send, is_state_epoch);
		}

		boost::json::object response;
		response["started"] = "1";
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (block_confirm_command);
}
