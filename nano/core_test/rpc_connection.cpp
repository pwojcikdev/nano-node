#include <nano/core_test/fakes/http_client.hpp>
#include <nano/core_test/fakes/rpc_handler.hpp>
#include <nano/core_test/fakes/strand_blocker.hpp>
#include <nano/lib/config.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/thread_runner.hpp>
#include <nano/rpc/rpc_connection.hpp>
#include <nano/rpc/rpc_host.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <chrono>
#include <deque>
#include <future>
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
 * A backend that throws is answered for with an internal error, whatever it threw, and the server goes on serving
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

/*
 * The connection as a unit, on a strand the test controls. A server that stops has to end the connections that
 * are not serving a request and spare the ones that are, and what decides a case is the order in which the stop
 * and the request reach the connection's strand, so these tests force that order instead of hoping for it.
 */

namespace
{
class connection_fixture final
{
public:
	explicit connection_fixture (nano::rpc_handler_interface & handler) :
		acceptor{ *io_ctx, boost::asio::ip::tcp::endpoint{ boost::asio::ip::address_v6::loopback (), 0 } },
		client{ acceptor.local_endpoint ().port () }
	{
		boost::asio::ip::tcp::socket socket{ strand };
		acceptor.accept (socket);
		connection = std::make_shared<nano::rpc_connection> (strand, std::move (socket), config, handler, logger);
	}

	~connection_fixture ()
	{
		connection->cancel ();
		runner.abort ();
		runner.join ();
	}

	void start ()
	{
		connection->start ([this] () {
			ended_promise.set_value ();
		});
	}

	bool ended_within (std::chrono::milliseconds time)
	{
		return ended.wait_for (time) == std::future_status::ready;
	}

	// Returns once everything queued on the strand so far has run
	void flush_strand ()
	{
		boost::asio::post (strand, boost::asio::use_future ([] () {})).wait ();
	}

	std::shared_ptr<boost::asio::io_context> io_ctx{ std::make_shared<boost::asio::io_context> () };
	nano::logger logger;
	nano::async::strand strand{ io_ctx->get_executor () };
	nano::rpc_config config{ nano::dev::network_params.network };
	boost::asio::ip::tcp::acceptor acceptor;
	nano::test::http_client client;
	std::shared_ptr<nano::rpc_connection> connection;
	std::promise<void> ended_promise;
	std::future<void> ended{ ended_promise.get_future () };
	nano::thread_runner runner{ io_ctx, logger, 2 }; // Last, so its threads are joined before anything they use goes away
};

std::string const echo_post = nano::test::http_post (R"({"action":"echo"})");
}

/**
 * Cancelling ends a connection even while it is serving, and the response that comes too late goes nowhere
 */
TEST (rpc_connection, cancel_while_serving)
{
	nano::test::system system;
	nano::test::deferred_rpc_handler handler;
	connection_fixture fixture{ handler };
	fixture.start ();

	ASSERT_TRUE (fixture.client.send (echo_post));
	ASSERT_TIMELY_EQ (5s, handler.pending_count (), 1);

	fixture.connection->cancel ();

	ASSERT_TRUE (fixture.ended_within (5s));
	ASSERT_TRUE (fixture.client.closed_by_server ());
	handler.respond_all (R"({"released":"too late"})");
	fixture.flush_strand ();
}

/**
 * Cancelling a connection that was started but has not run its first instruction yet is not lost: the
 * connection ends, its socket is closed and the request already waiting in it is not served
 */
TEST (rpc_connection, cancel_before_first_instruction)
{
	nano::test::echo_rpc_handler handler;
	connection_fixture fixture{ handler };
	ASSERT_TRUE (fixture.client.send (echo_post));

	nano::test::strand_blocker blocker{ fixture.strand };
	fixture.start (); // Queues up behind the blocker
	blocker.release ([&fixture] () {
		fixture.connection->cancel ();
	});

	ASSERT_TRUE (fixture.ended_within (5s));
	ASSERT_EQ (0, handler.requests);
	ASSERT_TRUE (fixture.client.closed_by_server ());
}

/**
 * A stop ends a connection that is waiting for its request
 */
TEST (rpc_connection, stop_if_idle_while_idle)
{
	nano::test::echo_rpc_handler handler;
	connection_fixture fixture{ handler };
	fixture.start ();

	fixture.connection->stop_if_idle ();

	ASSERT_TRUE (fixture.ended_within (5s));
	ASSERT_TRUE (fixture.client.closed_by_server ());
	ASSERT_EQ (0, handler.requests);
}

/**
 * A stop spares a connection that is serving a request, which is then answered as if nothing had happened
 */
TEST (rpc_connection, stop_if_idle_while_serving)
{
	nano::test::system system;
	nano::test::deferred_rpc_handler handler;
	connection_fixture fixture{ handler };
	fixture.start ();

	ASSERT_TRUE (fixture.client.send (echo_post));
	ASSERT_TIMELY_EQ (5s, handler.pending_count (), 1);

	fixture.connection->stop_if_idle ();
	fixture.flush_strand ();
	ASSERT_FALSE (fixture.ended_within (200ms));

	std::string const released = R"({"released":"1"})";
	handler.respond_all (released);
	auto response = fixture.client.read_response ();
	ASSERT_TRUE (response);
	ASSERT_EQ (http::status::ok, response->result ());
	ASSERT_EQ (released, response->body ());
	ASSERT_TRUE (fixture.ended_within (5s));
}

/**
 * A stop ends a connection that has read the header of its request and is waiting for the body
 */
TEST (rpc_connection, stop_if_idle_while_reading_body)
{
	nano::test::echo_rpc_handler handler;
	connection_fixture fixture{ handler };
	fixture.start ();

	ASSERT_TRUE (fixture.client.send ("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n"));
	fixture.flush_strand ();
	fixture.connection->stop_if_idle ();

	ASSERT_TRUE (fixture.ended_within (5s));
	ASSERT_EQ (0, handler.requests);
	ASSERT_TRUE (fixture.client.closed_by_server ());
}

/**
 * The race the gate exists for: the request has been read in full, but the stop reaches the strand before the
 * connection resumes from its last read. The request must not be served, although nothing failed while reading it.
 * The client asks before sending its body, so the interim response tells the test that the connection has moved on
 * to reading the body. The strand is then blocked while the body arrives, so the completed read queues up behind
 * the blocker, and the stop runs on the strand ahead of it.
 */
TEST (rpc_connection, stop_if_idle_before_resuming_from_read)
{
	nano::test::echo_rpc_handler handler;
	connection_fixture fixture{ handler };
	fixture.start ();

	std::string const body = R"({"action":"echo"})";
	ASSERT_TRUE (fixture.client.send ("POST / HTTP/1.1\r\nHost: localhost\r\nExpect: 100-continue\r\nContent-Length: " + std::to_string (body.size ()) + "\r\n\r\n"));
	auto interim = fixture.client.read_response ();
	ASSERT_TRUE (interim);
	ASSERT_EQ (http::status::continue_, interim->result ());

	nano::test::strand_blocker blocker{ fixture.strand }; // Occupies the strand only once the connection is suspended in the body read
	ASSERT_TRUE (fixture.client.send (body));
	std::this_thread::sleep_for (250ms); // The other IO thread completes the read, which cannot resume the connection yet
	blocker.release ([&fixture] () {
		fixture.connection->stop_if_idle ();
	});

	ASSERT_TRUE (fixture.ended_within (5s));
	ASSERT_EQ (0, handler.requests);
	ASSERT_TRUE (fixture.client.closed_by_server ());
}

/**
 * A stop that lands after a connection was started but before it ran its first instruction is not lost: the
 * connection ends without serving the request already waiting in its socket
 */
TEST (rpc_connection, stop_if_idle_before_first_instruction)
{
	nano::test::echo_rpc_handler handler;
	connection_fixture fixture{ handler };
	ASSERT_TRUE (fixture.client.send (echo_post));

	nano::test::strand_blocker blocker{ fixture.strand };
	fixture.start (); // Queues up behind the blocker
	blocker.release ([&fixture] () {
		fixture.connection->stop_if_idle ();
	});

	ASSERT_TRUE (fixture.ended_within (5s));
	ASSERT_EQ (0, handler.requests);
	ASSERT_TRUE (fixture.client.closed_by_server ());
}
