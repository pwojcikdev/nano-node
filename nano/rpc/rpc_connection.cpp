#include <nano/lib/json_error_response.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/rpc_handler_interface.hpp>
#include <nano/lib/rpcconfig.hpp>
#include <nano/lib/utility.hpp>
#include <nano/rpc/rpc_connection.hpp>
#include <nano/rpc/rpc_dispatcher.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <chrono>

namespace http = boost::beast::http;

namespace
{
std::string json_error (std::string const & message)
{
	std::string result;
	nano::json_error_response ([&result] (std::string const & body) { result = body; }, message);
	return result;
}
}

nano::rpc_connection::rpc_connection (nano::async::strand strand_a, asio::ip::tcp::socket socket, nano::rpc_config const & config_a, nano::rpc_handler_interface & rpc_handler_interface_a, nano::logger & logger_a) :
	config{ config_a },
	rpc_handler_interface{ rpc_handler_interface_a },
	logger{ logger_a },
	strand{ std::move (strand_a) },
	stream{ std::move (socket) },
	cancellation{ strand }
{
}

void nano::rpc_connection::start (std::function<void ()> on_done)
{
	// The completion handler keeps the connection alive for as long as its coroutine runs
	asio::co_spawn (strand, run (), asio::bind_cancellation_slot (cancellation.slot (), [this_s = shared_from_this (), on_done = std::move (on_done)] (std::exception_ptr const &) {
		// A connection cancelled before its first instruction never enters `run`, so the socket is closed here, on every path
		debug_assert (this_s->strand.running_in_this_thread ());
		this_s->stream.close ();
		on_done ();
	}));
}

void nano::rpc_connection::stop_if_idle ()
{
	asio::dispatch (strand, [this_s = shared_from_this ()] () {
		if (!this_s->serving)
		{
			this_s->cancellation.emit ();
		}
	});
}

void nano::rpc_connection::cancel ()
{
	cancellation.emit ();
}

asio::awaitable<void> nano::rpc_connection::run ()
{
	debug_assert (strand.running_in_this_thread ());
	try
	{
		co_await serve ();
	}
	catch (boost::system::system_error const & ex)
	{
		// A cancelled connection ends here from whichever operation it was waiting in
		if (ex.code () != asio::error::operation_aborted)
		{
			logger.error (nano::log::type::rpc_connection, "RPC request {} failed: {}", request_id (), ex.code ().message ());
		}
	}
	catch (std::exception const & ex)
	{
		// Whatever goes wrong while serving one request costs that connection, nothing else
		logger.error (nano::log::type::rpc_connection, "RPC request {} failed: {}", request_id (), ex.what ());
	}
	catch (...)
	{
		logger.error (nano::log::type::rpc_connection, "RPC request {} failed: unknown error", request_id ());
	}
}

asio::awaitable<void> nano::rpc_connection::serve ()
{
	boost::beast::flat_buffer buffer;
	http::request_parser<http::string_body> parser;
	parser.body_limit (config.max_request_size);

	auto [header_ec, header_size] = co_await http::async_read_header (stream, buffer, parser, asio::as_tuple (asio::use_awaitable));
	if (header_ec == http::error::end_of_stream || header_ec == asio::error::operation_aborted)
	{
		co_return; // The peer left without sending a request, or the connection was cancelled
	}

	bool const malformed = !!header_ec; // Answered with the reason
	if (!malformed)
	{
		if (boost::iequals (parser.get ()[http::field::expect], "100-continue"))
		{
			http::response<http::empty_body> interim{ http::status::continue_, 11 };
			interim.set (http::field::server, "nano");
			auto [interim_ec, interim_size] = co_await http::async_write (stream, interim, asio::as_tuple (asio::use_awaitable));
			if (interim_ec)
			{
				co_return;
			}
		}

		auto [body_ec, body_size] = co_await http::async_read (stream, buffer, parser, asio::as_tuple (asio::use_awaitable));
		if (body_ec)
		{
			if (body_ec != asio::error::operation_aborted)
			{
				logger.error (nano::log::type::rpc_connection, "RPC read error: {}", body_ec.message ());
			}
			co_return;
		}
	}

	// The gate: a stop may have run on this strand while the completed read waited to resume. The next co_await would throw as well, this does not depend on it; nothing suspends between here and the flag
	if ((co_await asio::this_coro::cancellation_state).cancelled () != asio::cancellation_type::none)
	{
		co_return;
	}
	serving = true;

	if (malformed)
	{
		logger.error (nano::log::type::rpc_connection, "RPC header error: {}", header_ec.message ());
		co_await write (make_response (11, json_error ("Invalid header: " + header_ec.message ())));
		co_return;
	}

	auto const & request = parser.get ();
	switch (request.method ())
	{
		case http::verb::post:
		{
			auto const start = std::chrono::steady_clock::now ();
			auto body = co_await process (request);

			// Bump logging level if RPC request logging is enabled
			logger.log (config.rpc_logging.log_rpc ? nano::log::level::info : nano::log::level::debug,
			nano::log::type::rpc_request, "RPC request {} completed in {} microseconds", request_id (), std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now () - start).count ());

			co_await write (make_response (request.version (), std::move (body)));
			break;
		}
		case http::verb::options:
		{
			co_await write (make_response (request.version (), {}));
			break;
		}
		default:
		{
			co_await write (make_response (request.version (), json_error ("Can only POST requests")));
			break;
		}
	}
}

asio::awaitable<std::string> nano::rpc_connection::process (request_type const & request)
{
	std::string const api_path = "/api/v2";
	std::string const path{ request.target () };

	nano::rpc_handler_request_params params;
	params.rpc_version = boost::starts_with (path, api_path) ? 2 : 1;
	params.credentials = std::string{ request["nano-api-key"] };
	params.correlation_id = std::string{ request["nano-correlation-id"] };
	params.path = boost::algorithm::erase_first_copy (path, api_path);
	params.path = boost::algorithm::erase_first_copy (params.path, "/");

	// A cancelled connection may be gone before the backend returns, so the call owns what it needs of the request
	co_return co_await nano::async::await_callback<std::string> (strand.get_inner_executor (), [&config_l = config, &rpc_handler_interface_l = rpc_handler_interface, &logger_l = logger, body = request.body (), id = request_id (), params] (auto respond) {
		try
		{
			auto dispatcher = std::make_shared<nano::rpc_dispatcher> (config_l, body, id, respond, rpc_handler_interface_l, logger_l);
			dispatcher->process_request (params);
		}
		catch (std::exception const & ex)
		{
			// An exception escaping here would end the IO thread running this; the connection sees its callback dropped instead
			logger_l.error (nano::log::type::rpc_connection, "RPC request {} could not be handed to the backend: {}", id, ex.what ());
		}
	});
}

asio::awaitable<void> nano::rpc_connection::write (response_type response)
{
	auto [ec, size] = co_await http::async_write (stream, response, asio::as_tuple (asio::use_awaitable));
	if (!ec)
	{
		boost::system::error_code ignored;
		stream.socket ().shutdown (asio::ip::tcp::socket::shutdown_send, ignored);
	}
}

auto nano::rpc_connection::make_response (unsigned version, std::string body) const -> response_type
{
	response_type response{ http::status::ok, version };
	response.set (http::field::allow, "POST, OPTIONS");
	response.set (http::field::content_type, "application/json");
	response.set (http::field::access_control_allow_origin, "*");
	response.set (http::field::access_control_allow_methods, "POST, OPTIONS");
	response.set (http::field::access_control_allow_headers, "Accept, Accept-Language, Content-Language, Content-Type");
	response.set (http::field::connection, "close");
	response.body () = std::move (body);
	response.prepare_payload ();
	return response;
}

std::string nano::rpc_connection::request_id () const
{
	return fmt::format ("{}", fmt::ptr (this));
}
