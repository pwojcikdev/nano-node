#pragma once

#include <nano/lib/block_uniquer.hpp>
#include <nano/lib/constants.hpp>
#include <nano/lib/endpoint.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/network_filter.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/stream.hpp>
#include <nano/lib/vote.hpp>
#include <nano/messages/messages.hpp>
#include <nano/transport/fwd.hpp>
#include <nano/transport/tcp_channel.hpp>
#include <nano/transport/tcp_socket.hpp>

namespace nano::transport
{
class tcp_server final : public std::enable_shared_from_this<tcp_server>
{
public:
	using disable_realtime_callback = std::function<bool ()>;
	using create_channel_callback = std::function<std::shared_ptr<nano::transport::tcp_channel> (std::shared_ptr<nano::transport::tcp_socket> const &, std::shared_ptr<nano::transport::tcp_server> const &, nano::account const &)>;
	using message_put_callback = std::function<bool (std::unique_ptr<nano::messages::message>, std::shared_ptr<nano::transport::tcp_channel> const &)>;

	tcp_server (asio::io_context &, nano::network_constants const &, nano::network_filter &, nano::block_uniquer &, nano::vote_uniquer &, nano::transport::handshake_provider &, nano::stats &, nano::logger &, disable_realtime_callback, create_channel_callback, message_put_callback, std::shared_ptr<nano::transport::tcp_socket>);
	~tcp_server ();

	void start ();

	void close ();
	void close_async (); // Safe to call from io context

	bool alive () const;

public:
	nano::endpoint get_remote_endpoint () const
	{
		return socket->get_remote_endpoint ();
	}
	nano::endpoint get_local_endpoint () const
	{
		return socket->get_local_endpoint ();
	}
	nano::transport::socket_type get_type () const
	{
		return socket->type ();
	}

private:
	enum class handshake_status
	{
		abort,
		realtime,
	};

	void stop ();

	asio::awaitable<void> start_impl ();
	asio::awaitable<handshake_status> perform_handshake ();
	asio::awaitable<void> run_realtime ();
	asio::awaitable<nano::deserialize_message_result> receive_message ();
	asio::awaitable<nano::deserialize_message_result> receive_message_impl ();
	asio::awaitable<nano::buffer_view> read_socket (size_t size) const;

private:
	asio::io_context & io_ctx;
	nano::network_constants const & network_constants;
	nano::network_filter & network_filter;
	nano::block_uniquer & block_uniquer;
	nano::vote_uniquer & vote_uniquer;
	nano::transport::handshake_provider & handshake;
	nano::stats & stats;
	nano::logger & logger;
	disable_realtime_callback disable_realtime;
	create_channel_callback create_channel;
	message_put_callback message_put;

	std::shared_ptr<nano::transport::tcp_socket> socket;
	std::shared_ptr<nano::transport::tcp_channel> channel; // Every realtime connection must have an associated channel

	nano::async::strand strand;
	nano::async::task task;

	nano::shared_buffer buffer;
	static size_t constexpr max_buffer_size = 64 * 1024; // 64 KB

private:
	bool to_realtime_connection (nano::account const & node_id);

private: // Visitors
	class realtime_message_visitor : public nano::messages::message_visitor
	{
	public:
		bool process{ false };

		void keepalive (nano::messages::keepalive const &) override;
		void publish (nano::messages::publish const &) override;
		void confirm_req (nano::messages::confirm_req const &) override;
		void confirm_ack (nano::messages::confirm_ack const &) override;
		void frontier_req (nano::messages::frontier_req const &) override;
		void telemetry_req (nano::messages::telemetry_req const &) override;
		void telemetry_ack (nano::messages::telemetry_ack const &) override;
		void asc_pull_req (nano::messages::asc_pull_req const &) override;
		void asc_pull_ack (nano::messages::asc_pull_ack const &) override;
	};
};
}
