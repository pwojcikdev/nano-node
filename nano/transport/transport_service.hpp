#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/lib/thread_runner.hpp>
#include <nano/secure/common.hpp>
#include <nano/transport/fwd.hpp>

#include <boost/asio/io_context.hpp>

#include <memory>

namespace asio = boost::asio;

namespace nano::transport
{
struct transport_params
{
	unsigned io_threads{ 1 };
};

class transport_service
{
public:
	transport_service (nano::network_params const & network_params, nano::stats & stats, nano::logger & logger, transport_params = {});
	~transport_service ();

	void start ();
	void stop ();

public:
	std::shared_ptr<asio::io_context> io_ctx_shared;
	asio::io_context & io_ctx;

	nano::network_params const network_params;
	nano::stats & stats;
	nano::logger & logger;

private:
	nano::thread_runner runner;
};
}
