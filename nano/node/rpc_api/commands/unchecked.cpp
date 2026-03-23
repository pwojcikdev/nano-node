#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/node/unchecked_map.hpp>
#include <nano/secure/common.hpp>

#include <boost/property_tree/json_parser.hpp>

using namespace nano::rpc_api;

class unchecked_command final : public sync_command
{
	std::string_view name () const override
	{
		return "unchecked";
	}
	command_result handle (request_context const & ctx) override
	{
		bool const json_block_l = params::optional_bool (ctx.params (), "json_block", false);
		auto count = params::optional_count (ctx.params ());

		boost::json::object unchecked;
		ctx.node.unchecked.for_each (
		[&unchecked, json_block_l] (nano::unchecked_key const & key, nano::unchecked_info const & info) {
			if (json_block_l)
			{
				boost::property_tree::ptree block_node_l;
				info.block->serialize_json (block_node_l);
				std::stringstream ss;
				boost::property_tree::write_json (ss, block_node_l);
				unchecked[info.block->hash ().to_string ()] = ss.str ();
			}
			else
			{
				std::string contents;
				info.block->serialize_json (contents);
				unchecked[info.block->hash ().to_string ()] = contents;
			}
		},
		[iterations = 0, count = count] () mutable { return iterations++ < count; });

		return command_result::ok ({ { "blocks", unchecked } });
	}
};
REGISTER_RPC_COMMAND (unchecked_command);

class unchecked_get_command final : public sync_command
{
	std::string_view name () const override
	{
		return "unchecked_get";
	}
	command_result handle (request_context const & ctx) override
	{
		bool const json_block_l = params::optional_bool (ctx.params (), "json_block", false);
		auto hash_r = params::required_hash (ctx.params ());
		if (!hash_r)
			return command_result::error (hash_r.error_message ());
		auto hash = *hash_r;

		boost::json::object result;
		bool done = false;
		ctx.node.unchecked.for_each (
		[&] (nano::unchecked_key const & key, nano::unchecked_info const & info) {
			if (key.hash == hash)
			{
				result["modified_timestamp"] = std::to_string (info.modified ());
				if (json_block_l)
				{
					boost::property_tree::ptree block_node_l;
					info.block->serialize_json (block_node_l);
					std::stringstream ss;
					boost::property_tree::write_json (ss, block_node_l);
					result["contents"] = ss.str ();
				}
				else
				{
					std::string contents;
					info.block->serialize_json (contents);
					result["contents"] = contents;
				}
				done = true;
			}
		},
		[&] () { return !done; });

		if (result.empty ())
			return command_result::error ("Block not found");
		return command_result::ok (std::move (result));
	}
};
REGISTER_RPC_COMMAND (unchecked_get_command);

class unchecked_keys_command final : public sync_command
{
	std::string_view name () const override
	{
		return "unchecked_keys";
	}
	command_result handle (request_context const & ctx) override
	{
		bool const json_block_l = params::optional_bool (ctx.params (), "json_block", false);
		auto count = params::optional_count (ctx.params ());
		nano::block_hash start_key (0);
		auto key_text = params::optional_string (ctx.params (), "key");
		if (!key_text.empty ())
		{
			if (start_key.decode_hex (key_text))
				return command_result::error ("Bad key");
		}

		boost::json::array unchecked;
		ctx.node.unchecked.for_each (
		start_key,
		[&unchecked, json_block_l] (nano::unchecked_key const & key, nano::unchecked_info const & info) {
			boost::json::object entry;
			entry["key"] = key.key ().to_string ();
			entry["hash"] = info.block->hash ().to_string ();
			entry["modified_timestamp"] = std::to_string (info.modified ());
			if (json_block_l)
			{
				boost::property_tree::ptree block_node_l;
				info.block->serialize_json (block_node_l);
				std::stringstream ss;
				boost::property_tree::write_json (ss, block_node_l);
				entry["contents"] = ss.str ();
			}
			else
			{
				std::string contents;
				info.block->serialize_json (contents);
				entry["contents"] = contents;
			}
			unchecked.emplace_back (std::move (entry));
		},
		[&unchecked, &count] () { return unchecked.size () < count; });

		return command_result::ok ({ { "unchecked", unchecked } });
	}
};
REGISTER_RPC_COMMAND (unchecked_keys_command);
