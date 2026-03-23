#include <nano/lib/container_info.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/stats_sinks.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/store/ledger_store.hpp>

#include <boost/property_tree/json_parser.hpp>

using namespace nano::rpc_api;

namespace
{
void construct_json (nano::container_info_component * component, boost::property_tree::ptree & parent)
{
	if (!component->is_composite ())
	{
		auto & leaf_info = static_cast<nano::container_info_leaf *> (component)->get_info ();
		boost::property_tree::ptree child;
		child.put ("count", leaf_info.count);
		child.put ("size", leaf_info.count * leaf_info.sizeof_element);
		parent.add_child (leaf_info.name, child);
		return;
	}

	auto composite = static_cast<nano::container_info_composite *> (component);
	boost::property_tree::ptree current;
	for (auto & child : composite->get_children ())
	{
		construct_json (child.get (), current);
	}
	parent.add_child (composite->get_name (), current);
}

// Convert a boost::property_tree to boost::json::object
boost::json::value ptree_to_json (boost::property_tree::ptree const & pt)
{
	// If the ptree has data but no children, it's a leaf value
	if (pt.empty () && !pt.data ().empty ())
	{
		return boost::json::value (pt.data ());
	}

	// Check if it looks like an array (all keys empty)
	bool is_array = !pt.empty ();
	for (auto const & child : pt)
	{
		if (!child.first.empty ())
		{
			is_array = false;
			break;
		}
	}

	if (is_array)
	{
		boost::json::array arr;
		for (auto const & child : pt)
		{
			arr.emplace_back (ptree_to_json (child.second));
		}
		return arr;
	}
	else
	{
		boost::json::object obj;
		for (auto const & child : pt)
		{
			obj[child.first] = ptree_to_json (child.second);
		}
		// If it also has data, include it
		if (!pt.data ().empty ())
		{
			obj[""] = pt.data ();
		}
		return obj;
	}
}
}

class stats_command final : public sync_command
{
	std::string_view name () const override
	{
		return "stats";
	}
	command_result handle (request_context const & ctx) override
	{
		auto type = params::optional_string (ctx.params (), "type");

		if (type == "counters")
		{
			nano::stat_json_writer sink;
			ctx.node.stats.log_counters (sink);
			auto stat_ptree = sink.to_ptree ();
			stat_ptree.put ("stat_duration_seconds", ctx.node.stats.last_reset ().count ());
			auto result = ptree_to_json (stat_ptree);
			return command_result::ok (result.as_object ());
		}
		else if (type == "samples")
		{
			nano::stat_json_writer sink;
			ctx.node.stats.log_samples (sink);
			auto stat_ptree = sink.to_ptree ();
			stat_ptree.put ("stat_duration_seconds", ctx.node.stats.last_reset ().count ());
			auto result = ptree_to_json (stat_ptree);
			return command_result::ok (result.as_object ());
		}
		else if (type == "objects")
		{
			boost::property_tree::ptree response_l;
			construct_json (ctx.node.container_info ().to_legacy ("node").get (), response_l);
			auto result = ptree_to_json (response_l);
			return command_result::ok (result.as_object ());
		}
		else if (type == "database")
		{
			boost::property_tree::ptree response_l;
			ctx.node.store.backend.collect_memory_stats (response_l);
			auto result = ptree_to_json (response_l);
			return command_result::ok (result.as_object ());
		}
		else
		{
			return command_result::error ("Invalid or missing type argument");
		}
	}
};
REGISTER_RPC_COMMAND (stats_command);

class stats_clear_command final : public sync_command
{
	std::string_view name () const override
	{
		return "stats_clear";
	}
	bool requires_control () const override
	{
		return true;
	}
	command_result handle (request_context const & ctx) override
	{
		ctx.node.stats.clear ();
		return command_result::ok ({ { "success", "" } });
	}
};
REGISTER_RPC_COMMAND (stats_clear_command);

class database_txn_tracker_command final : public sync_command
{
	std::string_view name () const override
	{
		return "database_txn_tracker";
	}
	bool requires_control () const override
	{
		return true;
	}
	command_result handle (request_context const & ctx) override
	{
		auto & p = ctx.params ();

		if (!ctx.node.config.txn_tracking.enable)
			return command_result::error ("Transaction tracking is not enabled");

		unsigned min_read_time_milliseconds = 0;
		auto min_read_str = params::optional_string (p, "min_read_time");
		if (!min_read_str.empty ())
		{
			try
			{
				min_read_time_milliseconds = std::stoul (min_read_str);
			}
			catch (...)
			{
				return command_result::error ("Invalid amount");
			}
		}

		unsigned min_write_time_milliseconds = 0;
		auto min_write_str = params::optional_string (p, "min_write_time");
		if (!min_write_str.empty ())
		{
			try
			{
				min_write_time_milliseconds = std::stoul (min_write_str);
			}
			catch (...)
			{
				return command_result::error ("Invalid amount");
			}
		}

		boost::property_tree::ptree json;
		ctx.node.store.backend.collect_txn_tracker (json, std::chrono::milliseconds (min_read_time_milliseconds), std::chrono::milliseconds (min_write_time_milliseconds));
		auto result = ptree_to_json (json);
		boost::json::object response;
		response["txn_tracking"] = std::move (result);
		return command_result::ok (std::move (response));
	}
};
REGISTER_RPC_COMMAND (database_txn_tracker_command);
