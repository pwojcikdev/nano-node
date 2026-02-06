#include <nano/lib/config.hpp>
#include <nano/lib/stream.hpp>
#include <nano/messages/messages.hpp>
#include <nano/transport/tcp/handshake.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <future>

namespace
{
class fixed_cookie_provider final : public nano::transport::handshake_provider
{
public:
	explicit fixed_cookie_provider (uint64_t cookie_a) :
		cookie{ nano::uint256_union{ cookie_a } }
	{
	}

	bool verify_handshake_response (nano::messages::node_id_handshake::response_payload const & response, nano::endpoint const &) override
	{
		return response.validate (cookie);
	}

	std::optional<nano::messages::node_id_handshake::query_payload> prepare_handshake_query (nano::endpoint const &) override
	{
		nano::messages::node_id_handshake::query_payload query{};
		query.cookie = cookie;
		return query;
	}

	nano::messages::node_id_handshake::response_payload prepare_handshake_response (nano::messages::node_id_handshake::query_payload const & query, bool) const override
	{
		nano::messages::node_id_handshake::response_payload response{};
		response.node_id = key.pub;
		response.sign (query.cookie, key);
		return response;
	}

	nano::messages::node_id_handshake::query_payload query_payload () const
	{
		nano::messages::node_id_handshake::query_payload query{};
		query.cookie = cookie;
		return query;
	}

	nano::account node_id () const
	{
		return key.pub;
	}

private:
	nano::uint256_union cookie;
	mutable nano::keypair key;
};

class scripted_handshake_io final
{
public:
	asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> read (nano::shared_buffer buffer, std::size_t target_size)
	{
		if (read_queue.empty ())
		{
			co_return std::tuple{ boost::asio::error::eof, 0 };
		}

		auto chunk = std::move (read_queue.front ());
		read_queue.pop_front ();
		if (chunk.size () != target_size)
		{
			co_return std::tuple{ boost::asio::error::fault, 0 };
		}

		std::copy (chunk.begin (), chunk.end (), buffer->begin ());
		co_return std::tuple{ boost::system::error_code{}, chunk.size () };
	}

	asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> write (nano::shared_const_buffer const & buffer)
	{
		written.emplace_back (buffer.to_bytes ());
		co_return std::tuple{ boost::system::error_code{}, buffer.size () };
	}

	void enqueue_message (nano::messages::message const & message)
	{
		auto bytes = message.to_shared_const_buffer ().to_bytes ();
		read_queue.emplace_back (bytes.begin (), bytes.begin () + nano::messages::message_header::size);
		read_queue.emplace_back (bytes.begin () + nano::messages::message_header::size, bytes.end ());
	}

	std::deque<std::vector<uint8_t>> read_queue;
	std::vector<std::vector<uint8_t>> written;
};

std::optional<nano::messages::node_id_handshake> deserialize_handshake (std::vector<uint8_t> const & bytes)
{
	if (bytes.size () < nano::messages::message_header::size)
	{
		return std::nullopt;
	}

	bool header_error = false;
	nano::bufferstream header_stream{ bytes.data (), nano::messages::message_header::size };
	nano::messages::message_header header{ header_error, header_stream };
	if (header_error || !header.is_valid_message_type () || header.type != nano::messages::message_type::node_id_handshake)
	{
		return std::nullopt;
	}

	nano::bufferstream payload_stream{ bytes.data () + nano::messages::message_header::size, bytes.size () - nano::messages::message_header::size };
	bool deserialize_error = false;
	nano::messages::node_id_handshake message{ deserialize_error, payload_stream, header };
	if (deserialize_error)
	{
		return std::nullopt;
	}

	return message;
}
}

TEST (transport_handshake_service, outbound_successful_handshake)
{
	fixed_cookie_provider outbound_provider{ 111 };
	fixed_cookie_provider inbound_provider{ 222 };

	scripted_handshake_io io;
	auto response_payload = inbound_provider.prepare_handshake_response (outbound_provider.query_payload (), false);
	nano::messages::node_id_handshake response{ nano::dev::network_params.network, std::nullopt, response_payload };
	io.enqueue_message (response);

	auto remote_endpoint = nano::endpoint{ boost::asio::ip::make_address_v6 ("::ffff:127.0.0.1"), 7075 };
	nano::transport::tcp::handshake_service service{ nano::dev::network_params.network, outbound_provider };

	boost::asio::io_context io_ctx;
	auto future = boost::asio::co_spawn (
	io_ctx, [&] () -> asio::awaitable<std::optional<nano::account>> {
		co_return co_await service.run (
		remote_endpoint, nano::transport::connection_type::outbound,
		[&io] (nano::shared_buffer buffer, std::size_t size) -> asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> {
			co_return co_await io.read (buffer, size);
		},
		[&io] (nano::shared_const_buffer const & buffer) -> asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> {
			co_return co_await io.write (buffer);
		});
	},
	boost::asio::use_future);

	io_ctx.run ();
	auto result = future.get ();

	ASSERT_TRUE (result.has_value ());
	ASSERT_EQ (*result, inbound_provider.node_id ());

	ASSERT_EQ (io.written.size (), 1);
	auto outbound_message = deserialize_handshake (io.written.front ());
	ASSERT_TRUE (outbound_message.has_value ());
	ASSERT_TRUE (outbound_message->query.has_value ());
	ASSERT_FALSE (outbound_message->response.has_value ());
}

TEST (transport_handshake_service, inbound_rejects_non_handshake_message)
{
	fixed_cookie_provider provider{ 333 };

	scripted_handshake_io io;
	nano::messages::keepalive keepalive{ nano::dev::network_params.network };
	io.enqueue_message (keepalive);

	auto remote_endpoint = nano::endpoint{ boost::asio::ip::make_address_v6 ("::ffff:127.0.0.1"), 7075 };
	nano::transport::tcp::handshake_service service{ nano::dev::network_params.network, provider };

	boost::asio::io_context io_ctx;
	auto future = boost::asio::co_spawn (
	io_ctx, [&] () -> asio::awaitable<std::optional<nano::account>> {
		co_return co_await service.run (
		remote_endpoint, nano::transport::connection_type::inbound,
		[&io] (nano::shared_buffer buffer, std::size_t size) -> asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> {
			co_return co_await io.read (buffer, size);
		},
		[&io] (nano::shared_const_buffer const & buffer) -> asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> {
			co_return co_await io.write (buffer);
		});
	},
	boost::asio::use_future);

	io_ctx.run ();
	auto result = future.get ();

	ASSERT_FALSE (result.has_value ());
	ASSERT_TRUE (io.written.empty ());
}
