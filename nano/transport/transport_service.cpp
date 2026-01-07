#include <nano/transport/transport_service.hpp>

namespace nano::transport
{
transport_service::transport_service (nano::network_params const & network_params_a, nano::stats & stats_a, nano::logger & logger_a, transport_params params) :
	io_ctx_shared{ std::make_shared<asio::io_context> () },
	io_ctx{ *io_ctx_shared },
	network_params{ network_params_a },
	stats{ stats_a },
	logger{ logger_a },
	runner{ io_ctx_shared, logger, params.io_threads }
{
}

transport_service::~transport_service ()
{
	stop ();
}

void transport_service::stop ()
{
	runner.abort ();
	runner.join ();
}
}
