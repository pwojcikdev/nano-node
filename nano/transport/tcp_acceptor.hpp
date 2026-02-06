#pragma once

#include <nano/lib/asio.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>

namespace asio = boost::asio;

namespace nano::transport
{
class tcp_acceptor
{
public:
	virtual ~tcp_acceptor () = default;

	virtual void open (uint16_t port) = 0;
	virtual void close (boost::system::error_code &) = 0;

	virtual bool is_open () const = 0;
	virtual asio::ip::tcp::endpoint local_endpoint () const = 0;

	virtual asio::awaitable<asio::ip::tcp::socket> async_accept () = 0;
};

class asio_tcp_acceptor final : public tcp_acceptor
{
public:
	explicit asio_tcp_acceptor (asio::any_io_executor);

	void open (uint16_t port) override;
	void close (boost::system::error_code &) override;

	bool is_open () const override;
	asio::ip::tcp::endpoint local_endpoint () const override;

	asio::awaitable<asio::ip::tcp::socket> async_accept () override;

private:
	asio::ip::tcp::acceptor acceptor;
};
}
