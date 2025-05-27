#pragma once

#include <nano/lib/async.hpp>
#include <nano/lib/enum_util.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/transport/fwd.hpp>
#include <nano/node/transport/message_queue.hpp>
#include <nano/node/transport/transport.hpp>

namespace nano::transport
{
class tcp_channel final : public nano::transport::channel, public std::enable_shared_from_this<tcp_channel>
{
public:
	tcp_channel (nano::node &, std::shared_ptr<nano::transport::tcp_socket>);
	~tcp_channel () override;

	void close () override;
	void close_async (); // Safe to call from io context

	bool max (nano::transport::traffic_type traffic_type) override;
	bool alive () const override;

	nano::endpoint get_remote_endpoint () const override;
	nano::endpoint get_local_endpoint () const override;

	nano::transport::transport_type get_type () const override
	{
		return nano::transport::transport_type::tcp;
	}

	std::string to_string () const override;

protected:
	bool send_impl (nano::message const &, nano::transport::traffic_type, nano::transport::channel::callback_t) override;

private:
	void start ();
	void stop ();

	asio::awaitable<void> start_sending (nano::async::condition &);
	asio::awaitable<void> run_sending (nano::async::condition &);
	asio::awaitable<boost::system::error_code> send_one (traffic_type, nano::transport::message_queue::entry_t const &);

public:
	std::shared_ptr<nano::transport::tcp_socket> socket;

private:
	nano::endpoint remote_endpoint;
	nano::endpoint local_endpoint;

	nano::async::strand strand;
	nano::async::task sending_task;

	mutable nano::mutex mutex;
	nano::transport::message_queue queue;
	std::atomic<size_t> allocated_bandwidth{ 0 };

public: // Logging
	void operator() (nano::object_stream &) const override;
};
}
