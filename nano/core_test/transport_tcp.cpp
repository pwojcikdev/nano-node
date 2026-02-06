#include <nano/lib/block_uniquer.hpp>
#include <nano/lib/config.hpp>
#include <nano/lib/network_filter.hpp>
#include <nano/lib/vote.hpp>
#include <nano/messages/messages.hpp>
#include <nano/transport/handshake.hpp>
#include <nano/transport/loopback.hpp>
#include <nano/transport/tcp_acceptor.hpp>
#include <nano/transport/tcp_channels.hpp>
#include <nano/transport/tcp_listener.hpp>
#include <nano/transport/tcp_server.hpp>

#include <gtest/gtest.h>

#include <deque>
#include <future>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

namespace
{
class fake_tcp_acceptor final : public nano::transport::tcp_acceptor
{
public:
	explicit fake_tcp_acceptor (asio::io_context & io_ctx_a) :
		io_ctx{ io_ctx_a }
	{
	}

	void open (uint16_t port_a) override
	{
		open_called = true;
		opened = true;
		open_port = port_a;
		auto const local_port = static_cast<uint16_t> (port_a == 0 ? 42424 : port_a);
		local = { asio::ip::address_v6::loopback (), local_port };
	}

	void close (boost::system::error_code & ec) override
	{
		opened = false;
		ec = {};
	}

	bool is_open () const override
	{
		return opened;
	}

	asio::ip::tcp::endpoint local_endpoint () const override
	{
		return local;
	}

	asio::awaitable<asio::ip::tcp::socket> async_accept () override
	{
		asio::steady_timer timer{ io_ctx };
		while (opened)
		{
			{
				std::lock_guard<std::mutex> lock{ mutex };
				if (!pending.empty ())
				{
					auto socket = std::move (pending.front ());
					pending.pop_front ();
					co_return std::move (socket);
				}
			}

			timer.expires_after (10ms);
			boost::system::error_code ec;
			co_await timer.async_wait (asio::redirect_error (asio::use_awaitable, ec));
			if (ec == asio::error::operation_aborted)
			{
				break;
			}
		}

		throw boost::system::system_error{ asio::error::operation_aborted };
	}

	void push_socket (asio::ip::tcp::socket socket_a)
	{
		std::lock_guard<std::mutex> lock{ mutex };
		pending.push_back (std::move (socket_a));
	}

public:
	bool open_called{ false };
	uint16_t open_port{ 0 };

private:
	asio::io_context & io_ctx;
	std::atomic<bool> opened{ false };
	asio::ip::tcp::endpoint local;
	std::deque<asio::ip::tcp::socket> pending;
	mutable std::mutex mutex;
};

asio::ip::tcp::socket make_connected_server_socket (asio::io_context & io_ctx)
{
	asio::ip::tcp::acceptor acceptor{ io_ctx, { asio::ip::address_v6::loopback (), 0 } };
	asio::ip::tcp::socket client{ io_ctx };
	client.connect ({ asio::ip::address_v6::loopback (), acceptor.local_endpoint ().port () });
	return acceptor.accept ();
}

template <typename Predicate>
bool wait_until (Predicate && predicate, std::chrono::milliseconds timeout = 2000ms)
{
	auto deadline = std::chrono::steady_clock::now () + timeout;
	while (std::chrono::steady_clock::now () < deadline)
	{
		if (predicate ())
		{
			return true;
		}
		std::this_thread::sleep_for (10ms);
	}
	return predicate ();
}

class test_handshake_provider final : public nano::transport::handshake_provider
{
public:
	bool verify_handshake_response (nano::messages::node_id_handshake::response_payload const &, nano::endpoint const &) override
	{
		return false;
	}

	std::optional<nano::messages::node_id_handshake::query_payload> prepare_handshake_query (nano::endpoint const &) override
	{
		return std::nullopt;
	}

	nano::messages::node_id_handshake::response_payload prepare_handshake_response (nano::messages::node_id_handshake::query_payload const & query, bool) const override
	{
		nano::messages::node_id_handshake::response_payload response;
		response.node_id = key.pub;
		response.sign (query.cookie, key);
		return response;
	}

private:
	mutable nano::keypair key;
};
}

TEST (transport_tcp_acceptor, accepts_connection)
{
	asio::io_context io_ctx;
	nano::transport::asio_tcp_acceptor acceptor{ io_ctx.get_executor () };

	acceptor.open (0);

	std::promise<asio::ip::tcp::socket> accepted_promise;
	auto accepted_future = accepted_promise.get_future ();
	asio::co_spawn (
	io_ctx, [&acceptor, &accepted_promise] () -> asio::awaitable<void> {
		try
		{
			auto accepted = co_await acceptor.async_accept ();
			accepted_promise.set_value (std::move (accepted));
		}
		catch (...)
		{
			accepted_promise.set_exception (std::current_exception ());
		}
	},
	asio::detached);
	std::thread io_thread{ [&io_ctx] () {
		io_ctx.run ();
	} };

	asio::io_context client_ctx;
	asio::ip::tcp::socket client{ client_ctx };
	client.connect ({ asio::ip::address_v6::loopback (), acceptor.local_endpoint ().port () });

	auto accepted = accepted_future.get ();
	ASSERT_TRUE (accepted.is_open ());
	ASSERT_EQ (accepted.local_endpoint ().port (), acceptor.local_endpoint ().port ());

	boost::system::error_code ec;
	acceptor.close (ec);
	ASSERT_FALSE (ec);

	io_ctx.stop ();
	io_thread.join ();
}

TEST (transport_tcp_listener, injected_acceptor_is_used)
{
	asio::io_context io_ctx;
	nano::logger logger{ "transport_test" };
	nano::stats stats{ logger };
	nano::transport::tcp_config config{ nano::dev::network_params.network };

	nano::transport::tcp_listener_params params;
	params.default_port = 17777;
	params.disable_max_peers_per_ip = true;
	params.disable_max_peers_per_subnetwork = true;

	auto acceptor = std::make_unique<fake_tcp_acceptor> (io_ctx);
	auto * acceptor_ptr = acceptor.get ();

	nano::transport::tcp_listener listener{
		io_ctx,
		0,
		config,
		stats,
		logger,
		params,
		[] (asio::ip::address const &) { return false; },
		[] (std::shared_ptr<nano::transport::tcp_socket> const &) { return std::shared_ptr<nano::transport::tcp_server>{}; },
		std::move (acceptor)
	};

	listener.start ();
	std::thread io_thread{ [&io_ctx] () {
		io_ctx.run ();
	} };

	ASSERT_TRUE (acceptor_ptr->open_called);
	ASSERT_EQ (acceptor_ptr->open_port, 0);
	ASSERT_EQ (listener.endpoint ().port (), 42424);

	listener.stop ();
	io_ctx.stop ();
	io_thread.join ();
}

TEST (transport_tcp_listener, accepts_connection_with_injected_server_factory)
{
	asio::io_context io_ctx;
	nano::logger logger{ "transport_test" };
	nano::stats stats{ logger };
	nano::transport::tcp_config config{ nano::dev::network_params.network };
	config.max_inbound_connections = 8;

	nano::transport::tcp_listener_params params;
	params.disable_max_peers_per_ip = true;
	params.disable_max_peers_per_subnetwork = true;

	auto acceptor = std::make_unique<fake_tcp_acceptor> (io_ctx);
	auto * acceptor_ptr = acceptor.get ();
	acceptor_ptr->push_socket (make_connected_server_socket (io_ctx));

	nano::network_filter network_filter{ 1024, 60 };
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer vote_uniquer;
	test_handshake_provider handshake;

	nano::transport::tcp_listener listener{
		io_ctx,
		0,
		config,
		stats,
		logger,
		params,
		[] (asio::ip::address const &) { return false; },
		[&io_ctx, &network_filter, &block_uniquer, &vote_uniquer, &handshake, &stats, &logger] (std::shared_ptr<nano::transport::tcp_socket> const & socket) {
			auto disable_realtime = [] () {
				return false;
			};
			auto create_channel = [] (std::shared_ptr<nano::transport::tcp_socket> const &, std::shared_ptr<nano::transport::tcp_server> const &, nano::account const &) {
				return std::shared_ptr<nano::transport::tcp_channel>{};
			};
			auto message_put = [] (std::unique_ptr<nano::messages::message>, std::shared_ptr<nano::transport::tcp_channel> const &) {
				return false;
			};
			return std::make_shared<nano::transport::tcp_server> (
			io_ctx,
			nano::dev::network_params.network,
			network_filter,
			block_uniquer,
			vote_uniquer,
			handshake,
			stats,
			logger,
			std::move (disable_realtime),
			std::move (create_channel),
			std::move (message_put),
			socket);
		},
		std::move (acceptor)
	};

	int accepted_events{ 0 };
	listener.connection_accepted.add ([&accepted_events] (auto const &, auto const &) {
		++accepted_events;
	});

	listener.start ();
	std::thread io_thread{ [&io_ctx] () {
		io_ctx.run ();
	} };

	ASSERT_TRUE (wait_until ([&accepted_events] () {
		return accepted_events >= 1;
	}));

	listener.stop ();
	io_ctx.stop ();
	io_thread.join ();
}

TEST (transport_tcp_channels, start_tcp_delegates_to_callback)
{
	asio::io_context io_ctx;
	nano::logger logger{ "transport_test" };
	nano::stats stats{ logger };

	nano::transport::tcp_channels_params params;
	params.disable_max_peers_per_ip = true;
	params.disable_max_peers_per_subnetwork = true;

	std::optional<nano::endpoint> started_endpoint;
	nano::transport::tcp_channels channels{
		io_ctx,
		nano::dev::network_params.network,
		stats,
		logger,
		params,
		[] (std::size_t, nano::transport::traffic_type) { return true; },
		[] (nano::endpoint const &, bool) { return false; },
		[] (nano::tcp_endpoint const &) { return false; },
		[] (std::array<nano::endpoint, 8> &) {},
		[&started_endpoint] (nano::endpoint const & endpoint) {
			started_endpoint = endpoint;
			return true;
		},
		[] (std::shared_ptr<nano::transport::tcp_channel> const &) {}
	};

	nano::endpoint endpoint{ asio::ip::address_v6::loopback (), 7075 };
	ASSERT_TRUE (channels.start_tcp (endpoint));
	ASSERT_TRUE (started_endpoint.has_value ());
	ASSERT_EQ (*started_endpoint, endpoint);
}

TEST (transport_tcp_channels, track_reachout_respects_policy)
{
	asio::io_context io_ctx;
	nano::logger logger{ "transport_test" };
	nano::stats stats{ logger };

	nano::transport::tcp_channels_params params;
	params.disable_max_peers_per_ip = true;
	params.disable_max_peers_per_subnetwork = true;

	nano::transport::tcp_channels channels{
		io_ctx,
		nano::dev::network_params.network,
		stats,
		logger,
		params,
		[] (std::size_t, nano::transport::traffic_type) { return true; },
		[] (nano::endpoint const &, bool) { return false; },
		[] (nano::tcp_endpoint const &) { return false; },
		[] (std::array<nano::endpoint, 8> &) {},
		[] (nano::endpoint const &) { return true; },
		[] (std::shared_ptr<nano::transport::tcp_channel> const &) {}
	};

	nano::endpoint endpoint{ asio::ip::address_v6::loopback (), 7001 };
	ASSERT_TRUE (channels.track_reachout (endpoint));
	ASSERT_FALSE (channels.track_reachout (endpoint)); // Duplicate attempt is rejected
}

TEST (transport_loopback_channel, send_invokes_inbound_and_callback)
{
	nano::logger logger{ "transport_test" };
	nano::stats stats{ logger };

	bool inbound_called{ false };
	bool callback_called{ false };
	bool callback_ok{ false };

	auto channel = std::make_shared<nano::transport::loopback_channel> (
	stats,
	nano::dev::network_params.network.protocol_version,
	nano::endpoint{ asio::ip::address_v6::loopback (), 17075 },
	nano::account{ 1234 },
	[&inbound_called] (nano::messages::message const &, std::shared_ptr<nano::transport::channel> const &) {
		inbound_called = true;
	},
	[] (nano::transport::channel::callback_t callback) {
		callback (boost::system::errc::make_error_code (boost::system::errc::success), 0);
	});

	nano::messages::keepalive message{ nano::dev::network_params.network };
	ASSERT_TRUE (channel->send (message, nano::transport::traffic_type::test, [&callback_called, &callback_ok] (boost::system::error_code const & ec, std::size_t) {
		callback_called = true;
		callback_ok = !ec;
	}));

	ASSERT_TRUE (inbound_called);
	ASSERT_TRUE (callback_called);
	ASSERT_TRUE (callback_ok);
	ASSERT_EQ (channel->get_node_id (), nano::account{ 1234 });
}
