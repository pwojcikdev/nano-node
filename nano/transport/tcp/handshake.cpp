#include <nano/lib/stream.hpp>
#include <nano/transport/tcp/handshake.hpp>
#include <nano/transport/transport.hpp>

#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <memory>
#include <vector>

namespace nano::transport::tcp
{
handshake_service::handshake_service (nano::network_constants const & network_constants_a, handshake_provider & handshake_a) :
	network_constants{ network_constants_a },
	handshake{ handshake_a }
{
}

asio::awaitable<std::optional<nano::account>> handshake_service::run (asio::ip::tcp::socket & socket, connection_type type) const
{
	auto const remote_tcp_endpoint = socket.remote_endpoint ();
	auto const remote_endpoint = nano::transport::map_tcp_to_endpoint (remote_tcp_endpoint);

	read_callback const read = [&socket] (nano::shared_buffer buffer, std::size_t size) -> asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> {
		co_return co_await asio::async_read (socket, asio::buffer (buffer->data (), size), asio::as_tuple (asio::use_awaitable));
	};

	write_callback const write = [&socket] (nano::shared_const_buffer const & buffer) -> asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> {
		co_return co_await asio::async_write (socket, buffer, asio::as_tuple (asio::use_awaitable));
	};

	co_return co_await run (remote_endpoint, type, read, write, {});
}

asio::awaitable<std::optional<nano::account>> handshake_service::run (nano::endpoint const & remote_endpoint, connection_type type, read_callback const & read, write_callback const & write, receive_observer const & on_receive) const
{
	handshake_protocol protocol{ network_constants, handshake, remote_endpoint, type };

	if (auto initial = protocol.start ())
	{
		co_await send (write, *initial);
	}

	// Node-id handshake is a maximum two-message exchange.
	for (int i = 0; i < handshake_protocol::max_handshake_messages; ++i)
	{
		auto message = co_await receive (read);
		if (!message)
		{
			co_return std::nullopt;
		}
		if (on_receive)
		{
			on_receive (*message);
		}

		auto const step = protocol.process (*message);
		if (step.abort)
		{
			co_return std::nullopt;
		}

		if (step.reply)
		{
			co_await send (write, *step.reply);
		}

		if (step.remote_node_id)
		{
			co_return step.remote_node_id;
		}
	}

	co_return std::nullopt;
}

asio::awaitable<std::optional<nano::messages::node_id_handshake>> handshake_service::receive (read_callback const & read) const
{
	auto header_buffer = std::make_shared<std::vector<uint8_t>> (nano::messages::message_header::size);
	auto [header_ec, header_size] = co_await read (header_buffer, header_buffer->size ());
	if (header_ec || header_size != header_buffer->size ())
	{
		co_return std::nullopt;
	}

	auto header_stream = nano::bufferstream{ header_buffer->data (), header_buffer->size () };

	bool header_error = false;
	nano::messages::message_header header{ header_error, header_stream };
	if (header_error || !header.is_valid_message_type () || header.type != nano::messages::message_type::node_id_handshake)
	{
		co_return std::nullopt;
	}
	if (header.network != network_constants.current_network || header.version_using < network_constants.protocol_version_min)
	{
		co_return std::nullopt;
	}

	auto const payload_size = header.payload_length_bytes ();
	auto payload_buffer = std::make_shared<std::vector<uint8_t>> (payload_size);
	if (payload_size > 0)
	{
		auto [payload_ec, payload_read] = co_await read (payload_buffer, payload_size);
		if (payload_ec || payload_read != payload_size)
		{
			co_return std::nullopt;
		}
	}

	auto payload_stream = nano::bufferstream{ payload_buffer->data (), payload_buffer->size () };
	bool deserialize_error = false;
	nano::messages::node_id_handshake handshake_message{ deserialize_error, payload_stream, header };
	if (deserialize_error)
	{
		co_return std::nullopt;
	}

	co_return handshake_message;
}

asio::awaitable<void> handshake_service::send (write_callback const & write, nano::messages::node_id_handshake const & message) const
{
	auto data = message.to_shared_const_buffer ();

	auto [ec, size] = co_await write (data);
	if (ec)
	{
		throw boost::system::system_error{ ec };
	}
	if (size != data.size ())
	{
		throw boost::system::system_error{ boost::system::error_code{ boost::system::errc::protocol_error, boost::system::system_category () } };
	}
}
}
