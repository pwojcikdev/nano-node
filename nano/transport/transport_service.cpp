#include <nano/transport/transport_service.hpp>

namespace nano::transport
{
transport_service::transport_service (nano::network_params const & network_params_a, tcp_config const & tcp_config_a, handshake_provider & handshake_a, nano::stats & stats_a, nano::logger & logger_a, transport_params params) :
	io_ctx_shared{ std::make_shared<asio::io_context> () },
	io_ctx{ *io_ctx_shared },
	network_params{ network_params_a },
	tcp_config_ref{ tcp_config_a },
	handshake{ handshake_a },
	stats{ stats_a },
	logger{ logger_a },
	tcp{ io_ctx, tcp_config_ref, network_params.network, handshake, stats, logger, tcp::tcp_service_params{ params.port } },
	runner{ io_ctx_shared, logger, params.io_threads }
{
}

transport_service::~transport_service ()
{
	stop ();
}

void transport_service::stop ()
{
	if (stopped.exchange (true))
	{
		return;
	}

	tcp.stop ();
	runner.abort ();
	runner.join ();
}

void transport_service::start ()
{
	tcp.start ();
}

bool transport_service::connect (asio::ip::tcp::endpoint const & endpoint_a)
{
	return tcp.connect (endpoint_a);
}

asio::ip::tcp::endpoint transport_service::endpoint () const
{
	return tcp.endpoint ();
}
}
