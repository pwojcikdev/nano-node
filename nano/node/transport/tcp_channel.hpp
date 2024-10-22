#pragma once

#include <nano/lib/async.hpp>
#include <nano/lib/enum_util.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/transport/fwd.hpp>
#include <nano/node/transport/transport.hpp>

namespace nano::transport
{
class tcp_channel_queue final
{
public:
	explicit tcp_channel_queue ();

	using callback_t = std::function<void (boost::system::error_code const &, std::size_t)>;
	using entry_t = std::pair<nano::shared_const_buffer, callback_t>;
	using value_t = std::pair<traffic_type, entry_t>;
	using batch_t = std::deque<value_t>;

	bool empty () const;
	size_t size () const;
	size_t size (traffic_type) const;
	void push (traffic_type, entry_t);
	value_t next ();
	batch_t next_batch (size_t max_count);

	bool max (traffic_type) const;
	bool full (traffic_type) const;

	constexpr static size_t max_size = 128;

private:
	void seek_next ();
	size_t priority (traffic_type) const;

	using queue_t = std::pair<traffic_type, std::deque<entry_t>>;
	nano::enum_array<traffic_type, queue_t> queues{};
	nano::enum_array<traffic_type, queue_t>::iterator current{ queues.end () };
	size_t counter{ 0 };
};

class tcp_channel final : public nano::transport::channel, public std::enable_shared_from_this<tcp_channel>
{
	friend class nano::transport::tcp_channels;

public:
	tcp_channel (nano::node &, std::shared_ptr<nano::transport::tcp_socket>);
	~tcp_channel () override;

	void update_endpoints ();

	using callback_t = std::function<void (boost::system::error_code const &, std::size_t)>;

	// TODO: investigate clang-tidy warning about default parameters on virtual/override functions
	bool send_buffer (nano::shared_const_buffer const &,
	callback_t const & callback = nullptr,
	nano::transport::buffer_drop_policy = nano::transport::buffer_drop_policy::limiter,
	nano::transport::traffic_type = nano::transport::traffic_type::generic)
	override;

	std::string to_string () const override;

	nano::endpoint get_remote_endpoint () const override
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return remote_endpoint;
	}

	nano::endpoint get_local_endpoint () const override
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return local_endpoint;
	}

	nano::transport::transport_type get_type () const override
	{
		return nano::transport::transport_type::tcp;
	}

	bool max (nano::transport::traffic_type traffic_type) override;

	bool alive () const override
	{
		return socket->alive ();
	}

	void close () override;

private:
	void start ();
	void stop ();

	asio::awaitable<void> run_sending ();
	asio::awaitable<void> send_one (traffic_type, tcp_channel_queue::entry_t const &);
	asio::awaitable<void> wait_avaialble_bandwidth (traffic_type, size_t size);
	asio::awaitable<void> wait_available_socket ();

public:
	std::shared_ptr<nano::transport::tcp_socket> socket;

private:
	nano::endpoint remote_endpoint;
	nano::endpoint local_endpoint;

	nano::async::strand strand;
	nano::async::task sending_task;
	nano::async::condition sending_condition;

	mutable nano::mutex mutex;
	tcp_channel_queue queue;

	std::atomic<size_t> allocated_bandwidth{ 0 };

public: // Logging
	void operator() (nano::object_stream &) const override;
};
}
