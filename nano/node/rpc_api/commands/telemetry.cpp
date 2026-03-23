#include <nano/lib/jsonconfig.hpp>
#include <nano/node/endpoint.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>
#include <nano/node/telemetry.hpp>

#include <boost/property_tree/json_parser.hpp>

using namespace nano::rpc_api;

namespace
{
// Convert a ptree to a boost::json::object
boost::json::object ptree_to_json (boost::property_tree::ptree const & pt)
{
	boost::json::object result;
	for (auto const & pair : pt)
	{
		if (pair.second.empty ())
		{
			result[pair.first] = pair.second.data ();
		}
		else
		{
			// Check if it's an array (keys are empty)
			bool is_array = true;
			for (auto const & child : pair.second)
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
				for (auto const & child : pair.second)
				{
					if (child.second.empty ())
					{
						arr.push_back (boost::json::value (child.second.data ()));
					}
					else
					{
						arr.push_back (ptree_to_json (child.second));
					}
				}
				result[pair.first] = arr;
			}
			else
			{
				result[pair.first] = ptree_to_json (pair.second);
			}
		}
	}
	return result;
}

// Flatten a ptree into the result object (top-level values only)
void ptree_flatten_into (boost::property_tree::ptree const & pt, boost::json::object & result)
{
	for (auto const & pair : pt)
	{
		if (pair.second.empty ())
		{
			result[pair.first] = pair.second.data ();
		}
		else
		{
			// Check if it's an array (keys are empty)
			bool is_array = true;
			for (auto const & child : pair.second)
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
				for (auto const & child : pair.second)
				{
					if (child.second.empty ())
					{
						arr.push_back (boost::json::value (child.second.data ()));
					}
					else
					{
						arr.push_back (ptree_to_json (child.second));
					}
				}
				result[pair.first] = arr;
			}
			else
			{
				result[pair.first] = ptree_to_json (pair.second);
			}
		}
	}
}
}

// telemetry: Get telemetry data
class telemetry_cmd final : public sync_command
{
	std::string_view name () const override
	{
		return "telemetry";
	}
	bool requires_control () const override
	{
		return false;
	}

	command_result handle (request_context const & ctx) override
	{
		auto address_text = params::optional_string (ctx.params (), "address");
		auto port_text = params::optional_string (ctx.params (), "port");

		if (!address_text.empty () || !port_text.empty ())
		{
			// Specific peer requested
			if (address_text.empty () || port_text.empty ())
			{
				return command_result::error ("Requires both port and address");
			}

			uint16_t port;
			if (nano::parse_port (port_text, port))
			{
				return command_result::error ("Invalid port");
			}

			boost::asio::ip::address address;
			if (nano::parse_address (address_text, address))
			{
				return command_result::error ("Invalid IP address");
			}

			nano::endpoint endpoint{ address, port };

			// Check if requesting local telemetry
			if (address.is_loopback () && port == ctx.node.network.endpoint ().port ())
			{
				auto telemetry_data = ctx.node.local_telemetry ();
				nano::jsonconfig config_l;
				auto err = telemetry_data.serialize_json (config_l, false);
				if (!err)
				{
					boost::json::object result;
					ptree_flatten_into (config_l.get_tree (), result);
					return command_result::ok (result);
				}
				return command_result::error ("Internal error");
			}

			auto maybe_telemetry = ctx.node.telemetry.get_telemetry (nano::transport::map_endpoint_to_v6 (endpoint));
			if (maybe_telemetry)
			{
				auto telemetry = *maybe_telemetry;
				nano::jsonconfig config_l;
				auto err = telemetry.serialize_json (config_l, false);
				if (!err)
				{
					boost::json::object result;
					ptree_flatten_into (config_l.get_tree (), result);
					return command_result::ok (result);
				}
				return command_result::error ("Internal error");
			}
			return command_result::error ("Peer not found");
		}

		// No specific peer - return consolidated or raw telemetry
		auto raw = params::optional_bool (ctx.params (), "raw", false);

		if (raw)
		{
			auto telemetry_responses = ctx.node.telemetry.get_all_telemetries ();
			boost::json::array metrics;
			for (auto & telemetry_metrics : telemetry_responses)
			{
				nano::jsonconfig config_l;
				auto err = telemetry_metrics.second.serialize_json (config_l, false);
				config_l.put ("address", telemetry_metrics.first.address ());
				config_l.put ("port", telemetry_metrics.first.port ());
				if (!err)
				{
					metrics.push_back (ptree_to_json (config_l.get_tree ()));
				}
				else
				{
					return command_result::error ("Internal error");
				}
			}
			return command_result::ok ({ { "metrics", metrics } });
		}
		else
		{
			// Default: local telemetry
			auto telemetry_data = ctx.node.local_telemetry ();
			nano::jsonconfig config_l;
			auto err = telemetry_data.serialize_json (config_l, false);
			if (!err)
			{
				boost::json::object result;
				ptree_flatten_into (config_l.get_tree (), result);
				return command_result::ok (result);
			}
			return command_result::error ("Internal error");
		}
	}
};
REGISTER_RPC_COMMAND (telemetry_cmd);
