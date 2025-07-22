#pragma once

#include <nano/lib/stream.hpp>
#include <nano/node/endpoint.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/messages.hpp>
#include <nano/node/transport/fwd.hpp>
#include <nano/node/transport/tcp_socket.hpp>

#include <atomic>

namespace nano::transport
{
class tcp_server final : public std::enable_shared_from_this<tcp_server>
{
public:
	tcp_server (nano::node &, std::shared_ptr<nano::transport::tcp_socket>);
	~tcp_server ();

	void start ();

	void close ();
	void close_async (); // Safe to call from io context

	bool alive () const;

	// void start ();
	// void stop ();

	// void initiate_handshake ();
	// void timeout ();

	// std::shared_ptr<nano::transport::tcp_socket> const socket;
	// nano::mutex mutex;
	// std::atomic<bool> stopped{ false };
	// std::atomic<bool> handshake_received{ false };
	// // Remote endpoint used to remove response channel even after socket closing
	// nano::tcp_endpoint remote_endpoint{ boost::asio::ip::address_v6::any (), 0 };
	// std::chrono::steady_clock::time_point last_telemetry_req{};

	nano::endpoint get_remote_endpoint () const
	{
		return socket->get_remote_endpoint ();
	}
	nano::endpoint get_local_endpoint () const
	{
		return socket->get_local_endpoint ();
	}

private:
	void stop ();

	enum class process_result
	{
		abort,
		progress,
	};

	asio::awaitable<void> start_impl ();
	asio::awaitable<process_result> perform_handshake ();
	asio::awaitable<void> run_realtime ();
	asio::awaitable<nano::deserialize_message_result> receive_message ();
	asio::awaitable<nano::buffer_view> read_socket (size_t size) const;

	enum class handshake_status
	{
		abort,
		handshake,
		realtime,
		bootstrap,
	};

	asio::awaitable<handshake_status> process_handshake (nano::node_id_handshake const & message);
	asio::awaitable<void> send_handshake_response (nano::node_id_handshake::query_payload const & query, bool v2);
	asio::awaitable<void> send_handshake_request ();

private:
	nano::node & node;

	std::shared_ptr<nano::transport::tcp_socket> socket;
	std::shared_ptr<nano::transport::tcp_channel> channel; // Every realtime connection must have an associated channel

	nano::async::strand strand;
	nano::async::task task;

	nano::shared_buffer buffer;
	static size_t constexpr max_buffer_size = 64 * 1024; // 64 KB

	std::atomic<bool> handshake_received{ false };

private:
	bool to_bootstrap_connection ();
	bool to_realtime_connection (nano::account const & node_id);
	bool is_undefined_connection () const;
	bool is_bootstrap_connection () const;
	bool is_realtime_connection () const;

private: // Visitors
	class realtime_message_visitor : public nano::message_visitor
	{
	public:
		bool process{ false };

		void keepalive (nano::keepalive const &) override;
		void publish (nano::publish const &) override;
		void confirm_req (nano::confirm_req const &) override;
		void confirm_ack (nano::confirm_ack const &) override;
		void frontier_req (nano::frontier_req const &) override;
		void telemetry_req (nano::telemetry_req const &) override;
		void telemetry_ack (nano::telemetry_ack const &) override;
		void asc_pull_req (nano::asc_pull_req const &) override;
		void asc_pull_ack (nano::asc_pull_ack const &) override;
	};
};
}
