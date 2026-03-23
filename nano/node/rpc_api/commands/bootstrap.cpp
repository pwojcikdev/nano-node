#include <nano/node/bootstrap/bootstrap_service.hpp>
#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>

#include <boost/json/parse.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace nano::rpc_api
{
// bootstrap
class bootstrap_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "bootstrap";
	}

	command_result handle (request_context const & ctx) override
	{
		auto address_str = params::required_string (ctx.params (), "address");
		if (!address_str)
			return command_result::error (address_str.error_message ());

		auto port_str = params::required_string (ctx.params (), "port");
		if (!port_str)
			return command_result::error (port_str.error_message ());

		boost::system::error_code address_ec;
		boost::asio::ip::make_address_v6 (*address_str, address_ec);
		if (!address_ec)
		{
			uint16_t port;
			if (!nano::parse_port (*port_str, port))
			{
				return command_result::error ("Legacy bootstrap is disabled");
			}
			else
			{
				return command_result::error ("Invalid port");
			}
		}
		else
		{
			return command_result::error ("Invalid IP address");
		}
	}
};
REGISTER_RPC_COMMAND (bootstrap_command);

// bootstrap_any
class bootstrap_any_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "bootstrap_any";
	}

	command_result handle (request_context const & ctx) override
	{
		return command_result::error ("Legacy bootstrap is disabled");
	}
};
REGISTER_RPC_COMMAND (bootstrap_any_command);

// bootstrap_lazy
class bootstrap_lazy_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "bootstrap_lazy";
	}

	command_result handle (request_context const & ctx) override
	{
		auto hash = params::required_hash (ctx.params ());
		if (!hash)
			return command_result::error (hash.error_message ());

		return command_result::error ("Lazy bootstrap is disabled");
	}
};
REGISTER_RPC_COMMAND (bootstrap_lazy_command);

// bootstrap_status
class bootstrap_status_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "bootstrap_status";
	}

	command_result handle (request_context const & ctx) override
	{
		auto status = ctx.node.bootstrap.status ();

		boost::json::object response;
		response["priorities"] = std::to_string (status.priorities);
		response["blocking"] = std::to_string (status.blocking);
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (bootstrap_status_command);

// bootstrap_priorities
class bootstrap_priorities_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "bootstrap_priorities";
	}

	command_result handle (request_context const & ctx) override
	{
		auto ptree = ctx.node.bootstrap.info ();
		// Convert boost::property_tree to boost::json by serializing and reparsing
		std::stringstream ss;
		boost::property_tree::write_json (ss, ptree);
		auto parsed = boost::json::parse (ss.str ());

		boost::json::object response;
		response["bootstrap"] = parsed;
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (bootstrap_priorities_command);

// bootstrap_reset
class bootstrap_reset_command : public sync_command
{
public:
	std::string_view name () const override
	{
		return "bootstrap_reset";
	}

	command_result handle (request_context const & ctx) override
	{
		ctx.node.bootstrap.reset ();
		boost::json::object response;
		response["success"] = "";
		return command_result::ok (response);
	}
};
REGISTER_RPC_COMMAND (bootstrap_reset_command);
}
