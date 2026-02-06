#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/lib/thread_runner.hpp>
#include <nano/secure/common.hpp>
#include <nano/transport/fwd.hpp>
#include <nano/transport/handshake.hpp>
#include <nano/transport/tcp/tcp_service.hpp>
#include <nano/transport/tcp_config.hpp>

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <memory>

namespace nano::transport
{
struct transport_params
{
	unsigned io_threads{ 1 };
	uint16_t port{ 0 };
};

class transport_service
{
public:
	transport_service (nano::network_params const & network_params, tcp_config const & tcp_config_a, handshake_provider & handshake, nano::stats & stats, nano::logger & logger, transport_params = {});
	~transport_service ();

	void start ();
	void stop ();
	bool connect (asio::ip::tcp::endpoint const & endpoint);
	asio::ip::tcp::endpoint endpoint () const;

public:
	std::shared_ptr<boost::asio::io_context> io_ctx_shared;
	boost::asio::io_context & io_ctx;

	nano::network_params const network_params;
	tcp_config const & tcp_config_ref;
	handshake_provider & handshake;
	nano::stats & stats;
	nano::logger & logger;
	tcp::tcp_service tcp;

private:
	nano::thread_runner runner;
	std::atomic<bool> stopped{ false };
};
}
