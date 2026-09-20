#pragma once

#include <nano/boost/asio/ip/tcp.hpp>
#include <nano/boost/asio/write.hpp>
#include <nano/boost/beast/core/flat_buffer.hpp>
#include <nano/boost/beast/http.hpp>

#include <boost/asio/io_context.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nano::test
{
/**
 * A blocking HTTP client that speaks raw bytes, for tests of the server side that must not depend on the RPC API.
 * It owns its io_context, so a test can hold a connection open, send a request in pieces or read until the server
 * closes. Every wait is bounded by a deadline.
 */
class http_client final
{
public:
	using response_type = boost::beast::http::response<boost::beast::http::string_body>;

	// Connects to the loopback interface, check `connected` for the outcome
	explicit http_client (uint16_t port)
	{
		boost::system::error_code ec;
		socket.connect (boost::asio::ip::tcp::endpoint{ boost::asio::ip::address_v6::loopback (), port }, ec);
		is_connected = !ec;
	}

	bool connected () const
	{
		return is_connected;
	}

	// Writes the bytes as they are, returns false if the connection did not take them
	bool send (std::string_view bytes)
	{
		boost::system::error_code ec;
		boost::asio::write (socket, boost::asio::buffer (bytes.data (), bytes.size ()), ec);
		return !ec;
	}

	// Reads one response, empty if none arrived in full before the deadline
	std::optional<response_type> read_response (std::chrono::milliseconds deadline = std::chrono::seconds{ 5 })
	{
		boost::beast::http::response_parser<boost::beast::http::string_body> parser;
		parser.body_limit (boost::none);

		bool done{ false };
		boost::system::error_code result;
		boost::beast::http::async_read (socket, buffer, parser, [&done, &result] (boost::system::error_code const & ec, std::size_t) {
			done = true;
			result = ec;
		});
		run_until (done, deadline);
		if (!done || result)
		{
			return std::nullopt;
		}
		return parser.release ();
	}

	// True once the server has closed its end of the connection
	bool closed_by_server (std::chrono::milliseconds deadline = std::chrono::seconds{ 5 })
	{
		bool done{ false };
		boost::system::error_code result;
		std::array<char, 1> byte{};
		socket.async_read_some (boost::asio::buffer (byte), [&done, &result] (boost::system::error_code const & ec, std::size_t) {
			done = true;
			result = ec;
		});
		run_until (done, deadline);
		return done && (result == boost::asio::error::eof || result == boost::asio::error::connection_reset);
	}

	// Closes this end, as a client that goes away would
	void close ()
	{
		boost::system::error_code ec;
		socket.close (ec);
	}

private:
	// Runs the pending operation; one that misses the deadline is cancelled and its handler still runs before returning
	void run_until (bool const & done, std::chrono::milliseconds deadline)
	{
		io_ctx.restart ();
		io_ctx.run_for (deadline);
		if (!done)
		{
			boost::system::error_code ec;
			socket.cancel (ec);
			io_ctx.restart ();
			io_ctx.run ();
		}
	}

	boost::asio::io_context io_ctx;
	boost::asio::ip::tcp::socket socket{ io_ctx };
	boost::beast::flat_buffer buffer; // Keeps what was read past the end of one response
	bool is_connected{ false };
};

// A POST request as a client would put it on the wire
inline std::string http_post (std::string const & body, std::string const & target = "/", std::string const & extra_headers = "", std::string const & version = "1.1")
{
	return "POST " + target + " HTTP/" + version + "\r\nHost: localhost\r\nContent-Type: application/json\r\n" + extra_headers + "Content-Length: " + std::to_string (body.size ()) + "\r\n\r\n" + body;
}
}
