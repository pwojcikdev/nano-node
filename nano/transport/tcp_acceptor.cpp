#include <nano/transport/tcp_acceptor.hpp>

#include <boost/asio/use_awaitable.hpp>

namespace nano::transport
{
asio_tcp_acceptor::asio_tcp_acceptor (asio::any_io_executor executor_a) :
	acceptor{ executor_a }
{
}

void asio_tcp_acceptor::open (uint16_t port)
{
	asio::ip::tcp::endpoint target{ asio::ip::address_v6::any (), port };

	acceptor.open (target.protocol ());
	acceptor.set_option (asio::ip::tcp::acceptor::reuse_address (true));
	acceptor.bind (target);
	acceptor.listen (asio::socket_base::max_listen_connections);
}

void asio_tcp_acceptor::close (boost::system::error_code & ec)
{
	acceptor.close (ec);
}

bool asio_tcp_acceptor::is_open () const
{
	return acceptor.is_open ();
}

asio::ip::tcp::endpoint asio_tcp_acceptor::local_endpoint () const
{
	return acceptor.local_endpoint ();
}

asio::awaitable<asio::ip::tcp::socket> asio_tcp_acceptor::async_accept ()
{
	co_return co_await acceptor.async_accept (asio::use_awaitable);
}
}
