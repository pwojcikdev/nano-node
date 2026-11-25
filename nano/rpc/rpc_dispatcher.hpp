#pragma once

#include <nano/lib/fwd.hpp>

#include <boost/property_tree/ptree.hpp>

#include <functional>
#include <string>

namespace nano
{
class rpc_config;
class rpc_handler_interface;
class rpc_handler_request_params;

/** Parses the JSON envelope of one request, enforces control-level access and hands it to the backend */
class rpc_dispatcher : public std::enable_shared_from_this<nano::rpc_dispatcher>
{
public:
	using response_callback = std::function<void (std::string const &, std::function<void ()>)>;
	rpc_dispatcher (nano::rpc_config const & rpc_config, std::string const & body_a, std::string const & request_id_a, response_callback const & response_a, nano::rpc_handler_interface & rpc_handler_interface_a, nano::logger &);
	void process_request (nano::rpc_handler_request_params const & request_params);

private:
	std::string body;
	std::string request_id;
	boost::property_tree::ptree request;
	response_callback send_response;
	nano::rpc_config const & rpc_config;
	nano::rpc_handler_interface & rpc_handler_interface;
	nano::logger & logger;
};
}
