#include <nano/core_test/fakes/http_client.hpp>
#include <nano/core_test/fakes/rpc_handler.hpp>
#include <nano/lib/config.hpp>
#include <nano/rpc/rpc_host.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;
namespace http = boost::beast::http;

/*
 * What a client sees of one HTTP connection to the RPC server: which requests are accepted, how they reach the
 * backend and what comes back. The backends know nothing about the RPC API and the client speaks raw HTTP, so
 * these tests describe the server's behaviour on the wire and nothing about how it is built.
 */

namespace
{
nano::rpc_config server_config (nano::test::system & system)
{
	nano::rpc_config config{ nano::dev::network_params.network, system.get_available_port (), true };
	config.rpc_process.io_threads = 2;
	return config;
}

std::string header (nano::test::http_client::response_type const & response, http::field field)
{
	return std::string{ response[field] };
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

void assert_response_headers (nano::test::http_client::response_type const & response)
{
	ASSERT_EQ ("application/json", header (response, http::field::content_type));
	ASSERT_EQ ("POST, OPTIONS", header (response, http::field::allow));
	ASSERT_EQ ("*", header (response, http::field::access_control_allow_origin));
	ASSERT_EQ ("POST, OPTIONS", header (response, http::field::access_control_allow_methods));
	ASSERT_EQ ("Accept, Accept-Language, Content-Language, Content-Type", header (response, http::field::access_control_allow_headers));
	ASSERT_EQ ("close", header (response, http::field::connection));
}
}

/**
 * A POST reaches the backend with its action and body, and the backend's response comes back with the
 * headers every response carries; the server then closes the connection
 */
TEST (rpc_connection, post)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	auto backend = std::make_unique<nano::test::echo_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	std::string const body = R"({"action":"echo","value":"1"})";
	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.connected ());
	ASSERT_TRUE (client.send (nano::test::http_post (body)));

	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (http::status::ok, response->result ());
	ASSERT_EQ (11, response->version ());
	ASSERT_EQ (body, response->body ());
	assert_response_headers (*response);

	auto const calls = handler.calls ();
	ASSERT_EQ (1, calls.size ());
	ASSERT_EQ (1, calls[0].rpc_version);
	ASSERT_EQ ("echo", calls[0].action);
	ASSERT_EQ (body, calls[0].body);

	ASSERT_TRUE (client.closed_by_server ());
}

/**
 * The response uses the HTTP version the request came in
 */
TEST (rpc_connection, post_http_10)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	host.start (std::make_unique<nano::test::echo_rpc_handler> ());

	std::string const body = R"({"action":"echo"})";
	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send (nano::test::http_post (body, "/", "", "1.0")));

	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (http::status::ok, response->result ());
	ASSERT_EQ (10, response->version ());
	ASSERT_EQ (body, response->body ());
}

/**
 * A POST below `/api/v2` reaches the backend as a version 2 call carrying the rest of the path, the API key
 * and the correlation id
 */
TEST (rpc_connection, post_v2)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	auto backend = std::make_unique<nano::test::echo_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	std::string const body = R"({"account":"1"})";
	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send (nano::test::http_post (body, "/api/v2/AccountWeight", "nano-api-key: secret\r\nnano-correlation-id: abc\r\n")));

	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (http::status::ok, response->result ());
	ASSERT_EQ (body, response->body ());
	assert_response_headers (*response);

	auto const calls = handler.calls ();
	ASSERT_EQ (1, calls.size ());
	ASSERT_EQ (2, calls[0].rpc_version);
	ASSERT_EQ (body, calls[0].body);
	ASSERT_EQ (2, calls[0].params.rpc_version);
	ASSERT_EQ ("AccountWeight", calls[0].params.path);
	ASSERT_EQ ("secret", calls[0].params.credentials);
	ASSERT_EQ ("abc", calls[0].params.correlation_id);
}

/**
 * OPTIONS is answered by the server itself, with the headers a browser's preflight request looks for
 */
TEST (rpc_connection, options)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	auto backend = std::make_unique<nano::test::echo_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send ("OPTIONS / HTTP/1.1\r\nHost: localhost\r\n\r\n"));

	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (http::status::ok, response->result ());
	ASSERT_TRUE (response->body ().empty ());
	assert_response_headers (*response);
	ASSERT_EQ (0, handler.requests);
	ASSERT_TRUE (client.closed_by_server ());
}

/**
 * Any other method is refused with a JSON error and never reaches the backend
 */
TEST (rpc_connection, unsupported_method)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	auto backend = std::make_unique<nano::test::echo_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send ("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"));

	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (http::status::ok, response->result ());
	ASSERT_EQ ("Can only POST requests", error_of (response->body ()));
	assert_response_headers (*response);
	ASSERT_EQ (0, handler.requests);
}

/**
 * A request that is not HTTP is answered with the reason as a JSON error
 */
TEST (rpc_connection, invalid_header)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	auto backend = std::make_unique<nano::test::echo_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send ("this is not http\r\n\r\n"));

	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (http::status::ok, response->result ());
	ASSERT_EQ (11, response->version ());
	ASSERT_TRUE (error_of (response->body ()).starts_with ("Invalid header: "));
	assert_response_headers (*response);
	ASSERT_EQ (0, handler.requests);
	ASSERT_TRUE (client.closed_by_server ());
}

/**
 * A request announcing a body above `max_request_size` is refused as soon as its header has been read
 */
TEST (rpc_connection, body_limit)
{
	nano::test::system system;
	auto config = server_config (system);
	config.max_request_size = 1024;
	nano::rpc_host host{ config };
	auto backend = std::make_unique<nano::test::echo_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	// Only the header is sent, the server must not wait for a body it is not going to accept
	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send ("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2048\r\n\r\n"));

	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ ("Invalid header: body limit exceeded", error_of (response->body ()));
	ASSERT_EQ (0, handler.requests);
	ASSERT_TRUE (client.closed_by_server ());

	// A body within the limit is served
	std::string const body = R"({"action":"echo"})";
	nano::test::http_client accepted{ host.listening_port () };
	ASSERT_TRUE (accepted.send (nano::test::http_post (body)));
	auto accepted_response = accepted.read_response ();
	ASSERT_TRUE (accepted_response);
	ASSERT_EQ (body, accepted_response->body ());
}

/**
 * A client that asks before sending its body is told to continue, and is then served as usual
 */
TEST (rpc_connection, expect_continue)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	host.start (std::make_unique<nano::test::echo_rpc_handler> ());

	std::string const body = R"({"action":"echo"})";
	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send ("POST / HTTP/1.1\r\nHost: localhost\r\nExpect: 100-continue\r\nContent-Length: " + std::to_string (body.size ()) + "\r\n\r\n"));

	auto interim = client.read_response ();
	ASSERT_TRUE (interim);
	ASSERT_EQ (http::status::continue_, interim->result ());
	ASSERT_EQ ("nano", header (*interim, http::field::server));

	ASSERT_TRUE (client.send (body));
	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (http::status::ok, response->result ());
	ASSERT_EQ (body, response->body ());
}

/**
 * A request that arrives in several pieces, as it may over a real network, is put together before it is served
 */
TEST (rpc_connection, request_in_pieces)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	host.start (std::make_unique<nano::test::echo_rpc_handler> ());

	std::string const body = R"({"action":"echo","value":"in pieces"})";
	auto const request = nano::test::http_post (body);
	std::deque<std::size_t> const cuts{ 10, request.size () - body.size () - 2, request.size () - body.size () + 5, request.size () };

	nano::test::http_client client{ host.listening_port () };
	std::size_t sent{ 0 };
	for (auto cut : cuts)
	{
		ASSERT_TRUE (client.send (std::string_view{ request }.substr (sent, cut - sent)));
		sent = cut;
		std::this_thread::sleep_for (50ms);
	}

	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (http::status::ok, response->result ());
	ASSERT_EQ (body, response->body ());
}

/**
 * Bodies far larger than a single read or write pass through intact in both directions
 */
TEST (rpc_connection, large_bodies)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	host.start (std::make_unique<nano::test::echo_rpc_handler> ());

	std::string const body = R"({"action":"echo","data":")" + std::string (4 * 1024 * 1024, 'x') + R"("})";
	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send (nano::test::http_post (body)));

	auto response = client.read_response (30s);
	ASSERT_TRUE (response);
	ASSERT_EQ (http::status::ok, response->result ());
	ASSERT_EQ (body.size (), response->body ().size ());
	ASSERT_TRUE (body == response->body ());
}

/**
 * A body that is not JSON is answered with an error by the dispatcher, from inside the call that hands it over
 */
TEST (rpc_connection, invalid_json)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	auto backend = std::make_unique<nano::test::echo_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send (nano::test::http_post ("this is not json")));

	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (http::status::ok, response->result ());
	ASSERT_EQ ("Unable to parse JSON", error_of (response->body ()));
	assert_response_headers (*response);
	ASSERT_EQ (0, handler.requests);
}

/**
 * JSON nested deeper than `max_json_depth` is refused before it is parsed
 */
TEST (rpc_connection, json_depth)
{
	nano::test::system system;
	auto config = server_config (system);
	config.max_json_depth = 4;
	nano::rpc_host host{ config };
	auto backend = std::make_unique<nano::test::echo_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send (nano::test::http_post (R"({"action":"echo","nested":[[[[[]]]]]})")));

	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ ("Max JSON depth exceeded", error_of (response->body ()));
	ASSERT_EQ (0, handler.requests);
}

/**
 * Only the first request on a connection is served, the server closes once it has responded
 */
TEST (rpc_connection, one_request_per_connection)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	auto backend = std::make_unique<nano::test::echo_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	// Both requests leave in a single write, so the server has read the second one by the time it closes
	std::string const first = R"({"action":"echo","value":"first"})";
	std::string const second = R"({"action":"echo","value":"second"})";
	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send (nano::test::http_post (first) + nano::test::http_post (second)));

	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (first, response->body ());
	ASSERT_TRUE (client.closed_by_server ());
	ASSERT_EQ (1, handler.requests);
}

/**
 * Requests on separate connections are in flight at the same time, and a backend may respond from a thread
 * of its own
 */
TEST (rpc_connection, concurrent_requests)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	auto backend = std::make_unique<nano::test::deferred_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	std::size_t const count = 16;
	std::deque<std::unique_ptr<nano::test::http_client>> clients;
	for (std::size_t i = 0; i < count; ++i)
	{
		clients.push_back (std::make_unique<nano::test::http_client> (host.listening_port ()));
		ASSERT_TRUE (clients.back ()->connected ());
		ASSERT_TRUE (clients.back ()->send (nano::test::http_post (R"({"action":"echo"})")));
	}

	// None of them has been responded to, so all of them are being served at once
	ASSERT_TIMELY_EQ (5s, handler.pending_count (), count);

	std::string const released = R"({"released":"1"})";
	handler.respond_all (released);
	for (auto const & client : clients)
	{
		auto response = client->read_response ();
		ASSERT_TRUE (response);
		ASSERT_EQ (http::status::ok, response->result ());
		ASSERT_EQ (released, response->body ());
	}
}

/**
 * A backend that throws is answered for with an error, and the server goes on serving
 */
TEST (rpc_connection, backend_throws)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	auto backend = std::make_unique<nano::test::throwing_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	for (int i = 0; i < 2; ++i)
	{
		nano::test::http_client client{ host.listening_port () };
		ASSERT_TRUE (client.send (nano::test::http_post (R"({"action":"echo"})")));
		auto response = client.read_response ();
		ASSERT_TRUE (response);
		ASSERT_EQ (http::status::ok, response->result ());
		ASSERT_EQ ("Internal server error in RPC", error_of (response->body ()));
	}
	ASSERT_EQ (2, handler.requests);
}

/**
 * A backend that lets go of a request without responding costs that connection, which is closed without a
 * response, and nothing else
 */
TEST (rpc_connection, backend_drops_request)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	auto backend = std::make_unique<nano::test::dropping_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	for (int i = 0; i < 2; ++i)
	{
		nano::test::http_client client{ host.listening_port () };
		ASSERT_TRUE (client.send (nano::test::http_post (R"({"action":"echo"})")));
		ASSERT_TRUE (client.closed_by_server ());
	}
	ASSERT_EQ (2, handler.requests);
}

/**
 * A client that goes away while its request is being served does not disturb the server
 */
TEST (rpc_connection, client_disconnects_before_response)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	auto backend = std::make_unique<nano::test::deferred_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	{
		nano::test::http_client client{ host.listening_port () };
		ASSERT_TRUE (client.send (nano::test::http_post (R"({"action":"echo"})")));
		ASSERT_TIMELY_EQ (5s, handler.pending_count (), 1);
		client.close ();
	}
	handler.respond_all (R"({"released":"nobody listens"})");

	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send (nano::test::http_post (R"({"action":"echo"})")));
	ASSERT_TIMELY_EQ (5s, handler.pending_count (), 1);
	std::string const released = R"({"released":"1"})";
	handler.respond_all (released);
	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (released, response->body ());
}

/**
 * A client that goes away in the middle of its request costs that connection only, and the part of the request
 * that did arrive never reaches the backend
 */
TEST (rpc_connection, client_disconnects_mid_request)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	auto backend = std::make_unique<nano::test::echo_rpc_handler> ();
	auto & handler = *backend;
	host.start (std::move (backend));

	{
		nano::test::http_client client{ host.listening_port () };
		ASSERT_TRUE (client.send ("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n{\"action\":"));
		client.close ();
	}

	std::string const body = R"({"action":"echo"})";
	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send (nano::test::http_post (body)));
	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (body, response->body ());
	ASSERT_EQ (1, handler.requests);
}

/**
 * Clients that connect and leave without sending anything do not disturb the server
 */
TEST (rpc_connection, client_disconnects_without_request)
{
	nano::test::system system;
	nano::rpc_host host{ server_config (system) };
	host.start (std::make_unique<nano::test::echo_rpc_handler> ());

	for (int i = 0; i < 8; ++i)
	{
		nano::test::http_client client{ host.listening_port () };
		ASSERT_TRUE (client.connected ());
		client.close ();
	}

	std::string const body = R"({"action":"echo"})";
	nano::test::http_client client{ host.listening_port () };
	ASSERT_TRUE (client.send (nano::test::http_post (body)));
	auto response = client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (body, response->body ());
}
