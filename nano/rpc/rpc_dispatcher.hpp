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

/**
 * Parses the JSON envelope of one request, enforces control-level access and hands it to the backend.
 * Every outcome is answered through the response: a request that cannot be parsed says so, and anything else
 * that goes wrong, in the backend included, is logged and answered as an internal error.
 */
class rpc_dispatcher : public std::enable_shared_from_this<nano::rpc_dispatcher>
{
public:
	rpc_dispatcher (nano::rpc_config const & rpc_config, std::string const & body_a, std::string const & request_id_a, std::function<void (std::string const &)> const & response_a, nano::rpc_handler_interface & rpc_handler_interface_a, nano::logger &);
	void process_request (nano::rpc_handler_request_params const & request_params);

private:
	void process_v1 ();
	void process_v2 (nano::rpc_handler_request_params const & request_params);
	// Whether the body nests deeper than `max_json_depth`, judged without parsing it
	bool exceeds_max_depth () const;
	// Whether the action needs `enable_control`, by its name or by a parameter of the request
	bool requires_control (std::string const & action, boost::property_tree::ptree const & request) const;

	std::string body;
	std::string request_id;
	std::function<void (std::string const &)> response;
	nano::rpc_config const & rpc_config;
	nano::rpc_handler_interface & rpc_handler_interface;
	nano::logger & logger;
};
}
