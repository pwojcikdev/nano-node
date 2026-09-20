#include <nano/core_test/fakes/rpc_handler.hpp>
#include <nano/lib/config.hpp>
#include <nano/lib/errors.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/rpcconfig.hpp>
#include <nano/rpc/rpc_dispatcher.hpp>
#include <nano/secure/network_params.hpp>

#include <gtest/gtest.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <deque>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>

/*
 * The dispatcher decides what becomes of one request before a backend sees it: whether it can be parsed, whether
 * it needs control to be enabled, and what the client is told when something fails. It is driven directly here,
 * with backends that know nothing about the RPC API, and every response it gives is collected.
 */

namespace
{
nano::rpc_config dispatcher_config (bool enable_control)
{
	return nano::rpc_config{ nano::dev::network_params.network, 0, enable_control };
}

// Every response the dispatcher gave to one request, in order
std::deque<std::string> dispatch (nano::rpc_config const & config, nano::rpc_handler_interface & handler, std::string const & body, nano::rpc_handler_request_params const & params = {})
{
	std::deque<std::string> responses;
	nano::logger logger;
	auto dispatcher = std::make_shared<nano::rpc_dispatcher> (
	config, body, "test", [&responses] (std::string const & response) { responses.push_back (response); }, handler, logger);
	dispatcher->process_request (params);
	return responses;
}

// The `error` field of a JSON error response, empty if the body is anything else
std::string error_of (std::string const & body)
{
	try
	{
		boost::property_tree::ptree tree;
		std::stringstream stream{ body };
		boost::property_tree::read_json (stream, tree);
		return tree.get<std::string> ("error", "");
	}
	catch (std::exception const &)
	{
		return {};
	}
}

std::string control_disabled_message ()
{
	std::error_code const ec = nano::error_rpc::rpc_control_disabled;
	return ec.message ();
}
}

/**
 * A request reaches the backend with its action and its body as they came, and the backend's answer goes back
 */
TEST (rpc_dispatcher, forwards_request)
{
	nano::test::echo_rpc_handler handler;
	std::string const body = R"({"action":"echo","value":"1"})";

	auto const responses = dispatch (dispatcher_config (false), handler, body);

	ASSERT_EQ (1, responses.size ());
	ASSERT_EQ (body, responses[0]);
	auto const calls = handler.calls ();
	ASSERT_EQ (1, calls.size ());
	ASSERT_EQ (1, calls[0].rpc_version);
	ASSERT_EQ ("echo", calls[0].action);
	ASSERT_EQ (body, calls[0].body);
}

/**
 * A version 2 request is not looked into, it reaches the backend with the parameters the connection took from it
 */
TEST (rpc_dispatcher, forwards_request_v2)
{
	nano::test::echo_rpc_handler handler;
	nano::rpc_handler_request_params params;
	params.rpc_version = 2;
	params.path = "AccountWeight";
	params.credentials = "secret";
	params.correlation_id = "abc";
	std::string const body = "not looked into";

	auto const responses = dispatch (dispatcher_config (false), handler, body, params);

	ASSERT_EQ (1, responses.size ());
	ASSERT_EQ (body, responses[0]);
	auto const calls = handler.calls ();
	ASSERT_EQ (1, calls.size ());
	ASSERT_EQ (2, calls[0].rpc_version);
	ASSERT_EQ (body, calls[0].body);
	ASSERT_EQ ("AccountWeight", calls[0].params.path);
	ASSERT_EQ ("secret", calls[0].params.credentials);
	ASSERT_EQ ("abc", calls[0].params.correlation_id);
}

/**
 * A body that is not JSON is answered with a parse error and never reaches the backend
 */
TEST (rpc_dispatcher, invalid_json)
{
	nano::test::echo_rpc_handler handler;

	auto const responses = dispatch (dispatcher_config (true), handler, "this is not json");

	ASSERT_EQ (1, responses.size ());
	ASSERT_EQ ("Unable to parse JSON", error_of (responses[0]));
	ASSERT_EQ (0, handler.requests);
}

/**
 * A request without an action gets the answer the node itself gives to one, and never reaches the backend
 */
TEST (rpc_dispatcher, missing_action)
{
	nano::test::echo_rpc_handler handler;

	auto const responses = dispatch (dispatcher_config (true), handler, R"({"account":"1"})");

	ASSERT_EQ (1, responses.size ());
	ASSERT_EQ ("Unable to parse JSON", error_of (responses[0]));
	ASSERT_EQ (0, handler.requests);
}

/**
 * JSON nested deeper than `max_json_depth` is refused without being parsed
 */
TEST (rpc_dispatcher, json_depth)
{
	nano::test::echo_rpc_handler handler;
	auto config = dispatcher_config (true);
	config.max_json_depth = 4;

	auto const refused = dispatch (config, handler, R"({"action":"echo","nested":[[[[[]]]]]})");
	ASSERT_EQ (1, refused.size ());
	ASSERT_EQ ("Max JSON depth exceeded", error_of (refused[0]));
	ASSERT_EQ (0, handler.requests);

	auto const accepted = dispatch (config, handler, R"({"action":"echo","nested":[[[]]]})");
	ASSERT_EQ (1, accepted.size ());
	ASSERT_TRUE (error_of (accepted[0]).empty ());
	ASSERT_EQ (1, handler.requests);
}

/**
 * An action that needs control is refused unless control is enabled, and one that does not is never refused
 */
TEST (rpc_dispatcher, control_disabled)
{
	std::string const controlled = R"({"action":"stop"})";
	std::string const harmless = R"({"action":"version"})";
	{
		nano::test::echo_rpc_handler handler;
		auto const responses = dispatch (dispatcher_config (false), handler, controlled);
		ASSERT_EQ (1, responses.size ());
		ASSERT_EQ (control_disabled_message (), error_of (responses[0]));
		ASSERT_EQ (0, handler.requests);
	}
	{
		nano::test::echo_rpc_handler handler;
		auto const responses = dispatch (dispatcher_config (true), handler, controlled);
		ASSERT_EQ (1, responses.size ());
		ASSERT_EQ (controlled, responses[0]);
		ASSERT_EQ (1, handler.requests);
	}
	{
		nano::test::echo_rpc_handler handler;
		auto const responses = dispatch (dispatcher_config (false), handler, harmless);
		ASSERT_EQ (1, responses.size ());
		ASSERT_EQ (harmless, responses[0]);
		ASSERT_EQ (1, handler.requests);
	}
}

/**
 * Two actions need control only for a value of one parameter. Without that value they reach the backend, and
 * so they do when the parameter is missing, which is the backend's to complain about and not a parse error.
 */
TEST (rpc_dispatcher, control_gated_parameters)
{
	auto const config = dispatcher_config (false);
	std::deque<std::string> const refused{
		R"({"action":"stats","type":"objects"})",
		R"({"action":"process","force":"true"})",
	};
	for (auto const & body : refused)
	{
		nano::test::echo_rpc_handler handler;
		auto const responses = dispatch (config, handler, body);
		ASSERT_EQ (1, responses.size ()) << body;
		ASSERT_EQ (control_disabled_message (), error_of (responses[0])) << body;
		ASSERT_EQ (0, handler.requests) << body;
	}

	std::deque<std::string> const forwarded{
		R"({"action":"stats","type":"counters"})",
		R"({"action":"stats"})",
		R"({"action":"process","force":"false"})",
		R"({"action":"process"})",
		R"({"action":"process","force":"not a boolean"})",
	};
	for (auto const & body : forwarded)
	{
		nano::test::echo_rpc_handler handler;
		auto const responses = dispatch (config, handler, body);
		ASSERT_EQ (1, responses.size ()) << body;
		ASSERT_EQ (body, responses[0]) << body;
		ASSERT_EQ (1, handler.requests) << body;
	}
}

/**
 * A `send` without an id is given one before it reaches the backend, so that a request that gets repeated is
 * not carried out twice; an id the client chose is left alone
 */
TEST (rpc_dispatcher, send_gets_id)
{
	auto const config = dispatcher_config (true);
	{
		nano::test::echo_rpc_handler handler;
		auto const responses = dispatch (config, handler, R"({"action":"send","amount":"1"})");
		ASSERT_EQ (1, responses.size ());
		boost::property_tree::ptree forwarded;
		std::stringstream stream{ handler.calls ().at (0).body };
		boost::property_tree::read_json (stream, forwarded);
		ASSERT_EQ ("send", forwarded.get<std::string> ("action"));
		ASSERT_EQ ("1", forwarded.get<std::string> ("amount"));
		ASSERT_EQ (32, forwarded.get<std::string> ("id").size ());
	}
	{
		nano::test::echo_rpc_handler handler;
		std::string const body = R"({"action":"send","id":"chosen"})";
		auto const responses = dispatch (config, handler, body);
		ASSERT_EQ (1, responses.size ());
		ASSERT_EQ (body, handler.calls ().at (0).body);
	}
}

/**
 * A backend that throws is a failure of the server, whatever it threw. In particular a `std::runtime_error`,
 * which is also what a JSON parse error is, must not be taken for one: the request was parsed without fault.
 */
TEST (rpc_dispatcher, backend_throws)
{
	auto const config = dispatcher_config (true);
	std::string const body = R"({"action":"echo"})";

	std::deque<std::function<void ()>> const failures{
		[] () { throw std::runtime_error ("backend failure"); },
		[] () { throw std::logic_error ("backend failure"); },
		[] () { throw std::bad_alloc (); },
		[] () { throw 42; },
	};
	for (auto const & failure : failures)
	{
		nano::test::throwing_rpc_handler handler{ failure };
		auto const responses = dispatch (config, handler, body);
		ASSERT_EQ (1, responses.size ());
		ASSERT_EQ ("Internal server error in RPC", error_of (responses[0]));
		ASSERT_EQ (1, handler.requests);
	}

	// The same for a version 2 request
	nano::rpc_handler_request_params params;
	params.rpc_version = 2;
	nano::test::throwing_rpc_handler handler;
	auto const responses = dispatch (config, handler, body, params);
	ASSERT_EQ (1, responses.size ());
	ASSERT_EQ ("Internal server error in RPC", error_of (responses[0]));
}
