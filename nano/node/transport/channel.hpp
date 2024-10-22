#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/object_stream.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/bandwidth_limiter.hpp>
#include <nano/node/common.hpp>
#include <nano/node/messages.hpp>
#include <nano/node/transport/tcp_socket.hpp>

#include <boost/asio/ip/network_v6.hpp>

namespace nano::transport
{
enum class transport_type : uint8_t
{
	undefined = 0,
	tcp = 1,
	loopback = 2,
	fake = 3
};

class channel
{
public:
	using callback_t = std::function<void (boost::system::error_code const &, std::size_t)>;

public:
	explicit channel (nano::node &);
	virtual ~channel () = default;

	void send (nano::message const &,
	callback_t const & callback = nullptr,
	nano::transport::buffer_drop_policy policy = nano::transport::buffer_drop_policy::limiter,
	nano::transport::traffic_type = nano::transport::traffic_type::generic);

	///	Implements the actual send operation
	/// Returns true if the message was sent (or queued to be sent), false if it was dropped
	// TODO: investigate clang-tidy warning about default parameters on virtual/override functions
	virtual bool send_buffer (nano::shared_const_buffer const &,
	callback_t const & callback = nullptr,
	nano::transport::buffer_drop_policy = nano::transport::buffer_drop_policy::limiter,
	nano::transport::traffic_type = nano::transport::traffic_type::generic)
	= 0;

	virtual void close () = 0;

	virtual nano::endpoint get_remote_endpoint () const = 0;
	virtual nano::endpoint get_local_endpoint () const = 0;

	virtual std::string to_string () const = 0;
	virtual nano::transport::transport_type get_type () const = 0;

	virtual bool max (nano::transport::traffic_type = nano::transport::traffic_type::generic)
	{
		return false;
	}

	virtual bool alive () const
	{
		return true;
	}

	std::chrono::steady_clock::time_point get_last_bootstrap_attempt () const
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return last_bootstrap_attempt;
	}

	void set_last_bootstrap_attempt (std::chrono::steady_clock::time_point const time_a)
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		last_bootstrap_attempt = time_a;
	}

	std::chrono::steady_clock::time_point get_last_packet_received () const
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return last_packet_received;
	}

	void set_last_packet_received (std::chrono::steady_clock::time_point const time_a)
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		last_packet_received = time_a;
	}

	std::chrono::steady_clock::time_point get_last_packet_sent () const
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return last_packet_sent;
	}

	void set_last_packet_sent (std::chrono::steady_clock::time_point const time_a)
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		last_packet_sent = time_a;
	}

	std::optional<nano::account> get_node_id_optional () const
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return node_id;
	}

	nano::account get_node_id () const
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return node_id.value_or (0);
	}

	void set_node_id (nano::account node_id_a)
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		node_id = node_id_a;
	}

	uint8_t get_network_version () const
	{
		return network_version;
	}

	void set_network_version (uint8_t network_version_a)
	{
		network_version = network_version_a;
	}

	nano::endpoint get_peering_endpoint () const;
	void set_peering_endpoint (nano::endpoint endpoint);

	std::shared_ptr<nano::node> owner () const;

protected:
	nano::node & node;
	mutable nano::mutex mutex;

private:
	std::chrono::steady_clock::time_point last_bootstrap_attempt{ std::chrono::steady_clock::time_point () };
	std::chrono::steady_clock::time_point last_packet_received{ std::chrono::steady_clock::now () };
	std::chrono::steady_clock::time_point last_packet_sent{ std::chrono::steady_clock::now () };
	std::optional<nano::account> node_id{};
	std::atomic<uint8_t> network_version{ 0 };
	std::optional<nano::endpoint> peering_endpoint{};

public: // Logging
	virtual void operator() (nano::object_stream &) const;
};
}