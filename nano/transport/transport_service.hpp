#pragma once

#include <nano/transport/fwd.hpp>
#include <nano/transport/transport_context.hpp>

#include <memory>

namespace nano::transport
{
class transport_service
{
public:
	transport_service (
	boost::asio::io_context &,
	nano::network_params const &,
	nano::stats &,
	nano::logger &,
	nano::transport::tcp_config const &,
	nano::transport::bandwidth_limiter &,
	uint16_t port);

	~transport_service ();

	void start ();
	void stop ();

	nano::container_info container_info () const;

private:
	std::unique_ptr<nano::transport::transport_context> ctx_impl;
	std::unique_ptr<nano::transport::tcp_channels> tcp_channels_impl;
	std::unique_ptr<nano::transport::tcp_listener> tcp_listener_impl;

public:
	nano::transport::transport_context & ctx;
	nano::transport::tcp_channels & tcp_channels;
	nano::transport::tcp_listener & tcp_listener;
};
}
