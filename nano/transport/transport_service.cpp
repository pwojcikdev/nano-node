#include <nano/transport/tcp_channels.hpp>
#include <nano/transport/tcp_listener.hpp>
#include <nano/transport/transport_service.hpp>

namespace nano::transport
{
transport_service::transport_service (boost::asio::io_context & io_ctx, nano::network_params const & network_params, nano::stats & stats, nano::logger & logger, tcp_config const & config, bandwidth_limiter & limiter, uint16_t port) :
	ctx_impl{ std::make_unique<transport_context> (transport_context{ io_ctx, network_params, stats, logger, config, limiter }) },
	tcp_channels_impl{ std::make_unique<nano::transport::tcp_channels> (*ctx_impl) },
	tcp_listener_impl{ std::make_unique<nano::transport::tcp_listener> (*ctx_impl, port) },
	ctx{ *ctx_impl },
	tcp_channels{ *tcp_channels_impl },
	tcp_listener{ *tcp_listener_impl }
{
}

transport_service::~transport_service ()
{
}

void transport_service::start ()
{
	tcp_channels.start ();
	tcp_listener.start ();
}

void transport_service::stop ()
{
	tcp_listener.stop ();
	tcp_channels.stop ();
}

nano::container_info transport_service::container_info () const
{
	nano::container_info info;
	info.add ("tcp_channels", tcp_channels.container_info ());
	info.add ("tcp_listener", tcp_listener.container_info ());
	return info;
}
}
