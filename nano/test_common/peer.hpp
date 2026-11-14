#pragma once

#include <nano/lib/asio.hpp>
#include <nano/lib/keypair.hpp>
#include <nano/lib/node_capabilities.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/messages/message.hpp>
#include <nano/messages/node_id_handshake.hpp>
#include <nano/node/endpoint.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/transport/common.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace nano::test
{
// What a test peer presents in its handshake
struct peer_options
{
	nano::keypair node_id{}; // Identity the peer signs its handshake with, a new one by default
	std::optional<uint8_t> protocol_version; // Protocol version of the peer's handshake messages, the node's own by default
	nano::node_capabilities_flags capabilities{}; // Capabilities announced in the handshake
};

/**
 * Stands in for a remote node on one TCP connection with a node under test.
 * It has a node id and nothing behind it: it sends only what the test tells it to and keeps every message the node sends it,
 * so the node's side of the connection runs production code from the listener to the message processor
 */
class peer final
{
public:
	static constexpr std::chrono::seconds timeout{ 5 }; // How long connecting, a send and each step of the handshake may take

	// Connects to the node's listener without starting the handshake, throws if no connection is made
	explicit peer (nano::node &, nano::test::peer_options const & = {});
	~peer ();

	nano::account node_id () const;

	// This peer's end of the connection, which the node's channel has as its remote endpoint
	nano::tcp_endpoint endpoint () const;

	// Exchanges handshake messages with the node, true once the node holds a channel for this peer
	bool handshake ();

	// This peer's signed answer to a handshake query carrying the cookie
	nano::messages::node_id_handshake::response_payload handshake_response (nano::uint256_union const & cookie) const;

	// Sends a message to the node, false if the connection is gone
	bool send (nano::messages::message const &);

	// Sends bytes as they are, for input that no message serializes to
	bool send_bytes (nano::buffer_view);

	// Messages of one type received from the node so far, in order of arrival
	template <typename Message>
	std::deque<Message> received () const;

	// Ends the connection from the peer's side
	void close ();

	// Whether the connection is open, false once either side has ended it
	bool alive () const;

private:
	friend class peer_listener;

	// Takes over a connection the node opened and a listener accepted
	peer (nano::node &, nano::test::peer_options const &, boost::asio::ip::tcp::socket accepted);

	// Starts the io thread, with no connection yet
	peer (nano::node &, nano::test::peer_options const &, nano::transport::connection_type);

	// Runs a coroutine on the io thread and waits for its result
	template <typename T>
	T run (boost::asio::awaitable<T>);

	boost::asio::awaitable<boost::system::error_code> connect ();
	boost::asio::awaitable<boost::system::error_code> write (nano::buffer_view);

	// Reads messages into the inbox until the connection ends
	boost::asio::awaitable<void> read_messages ();

	// Marks the connected socket open and starts reading from it
	void start_reading ();

	// Waits for the condition, false if it does not come true within the timeout
	bool wait_until (std::function<bool ()> const & condition) const;

	// A handshake message stamped with this peer's protocol version
	nano::messages::node_id_handshake make_handshake (std::optional<nano::messages::node_id_handshake::query_payload> const &, std::optional<nano::messages::node_id_handshake::response_payload> const &) const;

	void stop ();

private:
	nano::node & node;
	nano::test::peer_options const config;
	nano::transport::connection_type const connection; // Which side opened the connection, as the node sees it

	// Cookie of this peer's handshake query, which the node's response has to sign
	nano::uint256_union const cookie;

	boost::asio::io_context io_ctx;
	boost::asio::executor_work_guard<boost::asio::io_context::executor_type> io_guard;
	boost::asio::ip::tcp::socket socket;
	nano::tcp_endpoint local_endpoint;

	mutable std::mutex mutex;

	// Every message received from the node, in order of arrival
	std::deque<std::shared_ptr<nano::messages::message const>> inbox;

	bool open{ false };

	// Runs the io context, so the peer keeps reading whatever the test thread does. Declared last, so it starts once every other member exists
	std::thread thread;
};

/**
 * Listens on a loopback port for connections the node under test opens, and hands each of them out as a peer.
 * The kernel completes a connection whether or not a test accepts it, so a listener nobody accepts from still holds the node's connections open
 */
class peer_listener final
{
public:
	explicit peer_listener (nano::node &);

	// Where the listener takes connections, for a test to point the node at
	nano::endpoint endpoint () const;

	// The next connection the node opened, as a peer that has not started the handshake. Null if none arrives within the peer timeout
	std::unique_ptr<nano::test::peer> accept (nano::test::peer_options const & = {});

private:
	nano::node & node;

	boost::asio::io_context io_ctx;
	boost::asio::ip::tcp::acceptor acceptor;
};
}

template <typename Message>
std::deque<Message> nano::test::peer::received () const
{
	std::deque<Message> result;
	std::lock_guard guard{ mutex };
	for (auto const & message : inbox)
	{
		if (auto const * typed = dynamic_cast<Message const *> (message.get ()))
		{
			result.push_back (*typed);
		}
	}
	return result;
}
