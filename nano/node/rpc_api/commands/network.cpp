#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>

#include <boost/lexical_cast.hpp>

using namespace nano::rpc_api;

class peers_command final : public sync_command
{
	std::string_view name () const override
	{
		return "peers";
	}
	command_result handle (request_context const & ctx) override
	{
		bool peer_details = params::optional_bool (ctx.params (), "peer_details");
		auto peers_list = ctx.node.network.list (std::numeric_limits<std::size_t>::max ());
		std::sort (peers_list.begin (), peers_list.end (), [] (auto const & lhs, auto const & rhs) {
			return lhs->get_remote_endpoint () < rhs->get_remote_endpoint ();
		});
		boost::json::object peers;
		for (auto const & channel : peers_list)
		{
			auto key = channel->to_string ();
			if (peer_details)
			{
				auto node_id_opt = channel->get_node_id_optional ();
				boost::json::object details{
					{ "protocol_version", std::to_string (channel->get_network_version ()) },
					{ "node_id", node_id_opt.has_value () ? node_id_opt.value ().to_node_id () : "" },
					{ "type", "tcp" },
					{ "peering", boost::lexical_cast<std::string> (channel->get_peering_endpoint ()) },
				};
				peers[key] = std::move (details);
			}
			else
			{
				peers[key] = std::to_string (channel->get_network_version ());
			}
		}
		return command_result::ok ({ { "peers", peers } });
	}
};
REGISTER_RPC_COMMAND (peers_command);

class keepalive_command final : public sync_command
{
	std::string_view name () const override
	{
		return "keepalive";
	}
	bool requires_control () const override
	{
		return true;
	}
	command_result handle (request_context const & ctx) override
	{
		auto address_text = params::required_string (ctx.params (), "address");
		if (!address_text)
			return command_result::error (address_text.error_message ());
		auto port_text = params::required_string (ctx.params (), "port");
		if (!port_text)
			return command_result::error (port_text.error_message ());
		uint16_t port;
		if (nano::parse_port (*port_text, port))
			return command_result::error ("Invalid port");
		ctx.node.network.reachout (*address_text, port);
		return command_result::ok ({ { "started", "1" } });
	}
};
REGISTER_RPC_COMMAND (keepalive_command);
