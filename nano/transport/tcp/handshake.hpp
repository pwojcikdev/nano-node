#pragma once

#include <nano/lib/asio.hpp>
#include <nano/lib/constants.hpp>
#include <nano/lib/endpoint.hpp>
#include <nano/messages/node_id_handshake.hpp>
#include <nano/transport/common.hpp>
#include <nano/transport/fwd.hpp>
#include <nano/transport/handshake.hpp>
#include <nano/transport/tcp/handshake_protocol.hpp>

#include <boost/asio/ip/tcp.hpp>

#include <functional>
#include <optional>

namespace nano::transport::tcp
{
class handshake_service
{
public:
	using read_callback = std::function<asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> (nano::shared_buffer, std::size_t)>;
	using write_callback = std::function<asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> (nano::shared_const_buffer const &)>;
	using receive_observer = std::function<void (nano::messages::node_id_handshake const &)>;

	handshake_service (nano::network_constants const &, handshake_provider &);

	asio::awaitable<std::optional<nano::account>> run (asio::ip::tcp::socket &, connection_type) const;
	asio::awaitable<std::optional<nano::account>> run (nano::endpoint const &, connection_type, read_callback const &, write_callback const &, receive_observer const & = {}) const;

private:
	asio::awaitable<std::optional<nano::messages::node_id_handshake>> receive (read_callback const &) const;
	asio::awaitable<void> send (write_callback const &, nano::messages::node_id_handshake const &) const;

private:
	nano::network_constants const & network_constants;
	handshake_provider & handshake;
};
}
