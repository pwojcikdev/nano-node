#include <nano/lib/async.hpp>
#include <nano/rpc_test/rpc_response.hpp>

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <atomic>
#include <sstream>

namespace http = boost::beast::http;

namespace nano::test
{
/*
 * State shared between the caller-facing handle and the coroutine performing the call.
 * The coroutine holds a reference for the duration of the call, so the handle may be dropped while in flight.
 */
struct rpc_call_state
{
	enum class result
	{
		pending,
		success,
		connect_error,
		write_error,
		read_error,
		http_error,
		parse_error,
	};

	explicit rpc_call_state (asio::io_context & io_ctx) :
		socket{ io_ctx }
	{
	}

	asio::ip::tcp::socket socket;
	boost::beast::flat_buffer buffer;
	http::request<http::string_body> request;
	http::response<http::string_body> response;
	boost::property_tree::ptree json;
	std::atomic<result> status{ result::pending }; // written last, synchronizes access to the fields above
};
}

namespace
{
using result = nano::test::rpc_call_state::result;

asio::awaitable<void> run_call (std::shared_ptr<nano::test::rpc_call_state> state, uint16_t port)
{
	auto endpoint = asio::ip::tcp::endpoint{ asio::ip::address_v6::loopback (), port };
	auto [ec_connect] = co_await state->socket.async_connect (endpoint, asio::as_tuple (asio::use_awaitable));
	if (ec_connect)
	{
		state->status = result::connect_error;
		co_return;
	}

	auto [ec_write, size_write] = co_await http::async_write (state->socket, state->request, asio::as_tuple (asio::use_awaitable));
	if (ec_write)
	{
		state->status = result::write_error;
		co_return;
	}

	auto [ec_read, size_read] = co_await http::async_read (state->socket, state->buffer, state->response, asio::as_tuple (asio::use_awaitable));
	if (ec_read)
	{
		state->status = result::read_error;
		co_return;
	}

	if (state->response.result () != http::status::ok)
	{
		state->status = result::http_error;
		co_return;
	}

	try
	{
		std::stringstream body{ state->response.body () };
		boost::property_tree::read_json (body, state->json);
		state->status = result::success;
	}
	catch (std::exception const &)
	{
		state->status = result::parse_error;
	}
}
}

nano::test::rpc_response::rpc_response (std::shared_ptr<rpc_call_state> state) :
	state{ std::move (state) }
{
}

bool nano::test::rpc_response::finished () const
{
	return state->status != rpc_call_state::result::pending;
}

bool nano::test::rpc_response::ok () const
{
	return state->status == rpc_call_state::result::success;
}

boost::property_tree::ptree const & nano::test::rpc_response::json () const
{
	debug_assert (ok ());
	return state->json;
}

boost::beast::http::response<boost::beast::http::string_body> const & nano::test::rpc_response::raw () const
{
	debug_assert (finished ());
	return state->response;
}

nano::test::rpc_response nano::test::rpc_post (boost::property_tree::ptree const & request, uint16_t port, boost::asio::io_context & io_ctx)
{
	auto state = std::make_shared<rpc_call_state> (io_ctx);

	std::stringstream ostream;
	boost::property_tree::write_json (ostream, request);
	state->request.method (http::verb::post);
	state->request.target ("/");
	state->request.version (11);
	state->request.body () = ostream.str ();
	state->request.prepare_payload ();

	asio::co_spawn (io_ctx, run_call (state, port), asio::detached);
	return rpc_response{ state };
}
