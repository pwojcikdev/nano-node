/*
 * Benchmarks of sending through nano::transport::tcp_channel end to end: how fast one channel, or several at once, writes messages to a peer and what each message costs.
 * An iteration sends a burst of messages to the channel of every benchmark thread and ends once each peer has read the last byte and every callback has run.
 * Reported per message: the throughput, the sender's time in the send call, and the heap allocations of the sender and of the whole process.
 * The CPU column is the CPU time of the whole process per iteration.
 *
 * The fixture is a dev network node with realtime TCP channels over loopback, each to a peer that reads and discards everything.
 * The limited cases set the outbound bandwidth limiter below the channels' own rate, so sends wait for bandwidth.
 *
 * With the node's default io thread count, waking threads costs more than the sends themselves and the results swing with the machine's load.
 * NANO_IO_THREADS=1 saturates a single io thread instead, so the unlimited cases measure what the send path costs that thread per message.
 */

#include <nano/benchmarks/allocations.hpp>
#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/config.hpp>
#include <nano/lib/keypair.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/rate_limiting.hpp>
#include <nano/lib/utility.hpp>
#include <nano/messages/messages.hpp>
#include <nano/node/bandwidth_limiter.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/tcp_channel.hpp>
#include <nano/node/transport/tcp_channels.hpp>
#include <nano/node/transport/tcp_config.hpp>
#include <nano/node/transport/tcp_server.hpp>
#include <nano/node/transport/tcp_socket.hpp>
#include <nano/secure/vote.hpp>
#include <nano/test_common/system.hpp>

#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

namespace
{
constexpr size_t channel_count = 4; // Channels the multi-channel cases send to at once, one per benchmark thread
constexpr size_t burst_size = 256; // Messages sent to each channel per iteration
constexpr size_t window_size = 64; // Messages a channel may have in flight, not yet read by its peer
constexpr size_t refill_size = 32; // Messages still in flight when a sender with a full window sends again, so it wakes once per refill
static_assert (window_size < nano::transport::tcp_channel_queue::full_size, "a full channel queue would drop sends");

nano::block_hash random_hash ()
{
	nano::block_hash hash;
	nano::random_pool::generate_block (hash.bytes.data (), hash.bytes.size ());
	return hash;
}

// The benchmarks' main leaves the network alone, a node must never start on another one from here
struct dev_network
{
	dev_network ()
	{
		nano::force_nano_dev_network ();
	}
};

// The far end of one connection: a thread reads and discards everything the node writes and counts it, so a sender can wait for its bytes to arrive
class reading_peer final
{
public:
	explicit reading_peer (boost::asio::ip::tcp::socket socket_a) :
		socket{ std::move (socket_a) },
		thread{ [this] () { run (); } }
	{
	}

	~reading_peer ()
	{
		boost::system::error_code ec;
		socket.shutdown (boost::asio::ip::tcp::socket::shutdown_both, ec); // Ends the blocking read
		thread.join ();
	}

	// Bytes read since the connection opened
	uint64_t received () const
	{
		return received_bytes.load ();
	}

	// Reads made since the connection opened
	uint64_t reads () const
	{
		return read_count.load (std::memory_order_relaxed);
	}

	// Blocks until `bytes` have been read in all
	void wait_received (uint64_t bytes)
	{
		std::unique_lock lock{ mutex };
		wake_at = bytes;
		condition.wait (lock, [&] () { return received_bytes.load () >= bytes || closed.load (); });
		wake_at = no_waiter;
		release_assert (!closed, "connection closed while sending");
	}

private:
	void run ()
	{
		std::vector<uint8_t> buffer (read_size);
		while (true)
		{
			boost::system::error_code ec;
			auto const size = socket.read_some (boost::asio::buffer (buffer), ec);
			if (ec)
			{
				break;
			}
			read_count.fetch_add (1, std::memory_order_relaxed);
			// Wakes the sender only once it has what it waits for, not on every read
			if (received_bytes.fetch_add (size) + size >= wake_at.load ())
			{
				notify ();
			}
		}
		closed = true;
		notify ();
	}

	void notify ()
	{
		// Taking the lock keeps the notification from landing between a waiter's check and its wait
		{
			std::lock_guard guard{ mutex };
		}
		condition.notify_one ();
	}

private:
	static constexpr size_t read_size = 256 * 1024;
	static constexpr uint64_t no_waiter = std::numeric_limits<uint64_t>::max ();

	boost::asio::ip::tcp::socket socket;
	std::atomic<uint64_t> received_bytes{ 0 };
	std::atomic<uint64_t> read_count{ 0 };
	std::atomic<uint64_t> wake_at{ no_waiter }; // Received bytes that wake the waiting sender
	std::atomic<bool> closed{ false };
	std::mutex mutex;
	std::condition_variable condition;
	std::thread thread; // Last, so it starts once everything it uses is constructed
};

class channel_fixture
{
public:
	channel_fixture ()
	{
		auto config = system.default_config ();
		// The peers never write, which the sockets would take for a dead connection
		config.tcp->io_timeout = std::chrono::seconds{ 0 };
		config.tcp->silent_timeout = std::chrono::seconds{ 0 };
		nano::node_flags flags;
		flags.disable_connection_cleanup = true; // The channels never receive anything, which would get them purged
		flags.disable_rep_crawler = true; // Keeps the node's own queries off the channels
		flags.disable_ongoing_bootstrap = true;
		node = system.add_node (config, flags);

		boost::asio::ip::tcp::acceptor acceptor{ node->io_ctx, boost::asio::ip::tcp::endpoint{ boost::asio::ip::address_v6::loopback (), 0 } };
		for (size_t i = 0; i < channel_count; ++i)
		{
			boost::asio::ip::tcp::socket client{ peer_io };
			client.connect (acceptor.local_endpoint ());
			auto socket = std::make_shared<nano::transport::tcp_socket> (*node, acceptor.accept (), nano::transport::socket_endpoint::server);
			auto server = std::make_shared<nano::transport::tcp_server> (*node, socket);
			server->start ();
			// Every channel claims its own node id, a shared one would make it a duplicate of the first
			nano::transport::peer_info const peer{ .node_id = nano::keypair{}.pub, .protocol_version = node->network_params.network.protocol_version };
			auto const created = node->network.tcp_channels.create (socket, server, peer);
			release_assert (created.channel != nullptr);
			channels.push_back (created.channel);
			peers.push_back (std::make_unique<reading_peer> (std::move (client)));
		}
	}

	// A vote on `hashes` random hashes, built once per size
	std::shared_ptr<nano::vote> const & vote (size_t hashes)
	{
		std::lock_guard guard{ mutex };
		auto & vote = votes[hashes];
		if (!vote)
		{
			std::vector<nano::block_hash> hashes_l;
			for (size_t i = 0; i < hashes; ++i)
			{
				hashes_l.push_back (random_hash ());
			}
			vote = std::make_shared<nano::vote> (rep.pub, rep.prv, 1, 0, hashes_l);
		}
		return vote;
	}

public:
	dev_network network;
	nano::test::system system;
	std::shared_ptr<nano::node> node;
	boost::asio::io_context peer_io; // Owns the peers' sockets, which only make blocking calls
	std::vector<std::unique_ptr<reading_peer>> peers;
	std::vector<std::shared_ptr<nano::transport::channel>> channels; // The node's end of each peer's connection, at the same index

private:
	nano::keypair rep;
	std::mutex mutex; // Benchmark threads ask for their vote at the same time, it is built once under this lock
	std::map<size_t, std::shared_ptr<nano::vote>> votes;
};

// Built on first use, connecting the channels takes a moment
channel_fixture & shared_fixture ()
{
	static channel_fixture instance;
	return instance;
}

// How a case sends
struct sending_options
{
	bool callbacks{ false }; // Passes every send a completion callback
	size_t limit_mib{ 0 }; // Outbound bandwidth limit in MiB/s, unlimited at zero
};

// The outbound limit of a case, with a small burst so the limit holds from the start of every run
nano::rate_limit outbound_limit (size_t limit_mib)
{
	if (limit_mib == 0)
	{
		return nano::rate_limit::unlimited ();
	}
	return { .rate = limit_mib * 1024 * 1024, .burst_ratio = 0.02 };
}

/*
 * Sends `burst_size` copies of the message per iteration to the channel of the benchmark thread, keeping at most `window_size` of them in flight.
 * An iteration ends once the peer has read them all and, with callbacks, every callback has run.
 */
void send_bursts (benchmark::State & state, nano::messages::message const & message, nano::transport::traffic_type type, sending_options const & options)
{
	auto & fixture = shared_fixture ();
	auto const thread = static_cast<size_t> (state.thread_index ());
	release_assert (thread < channel_count);
	auto & channel = *fixture.channels[thread];
	auto & peer = *fixture.peers[thread];
	auto const size = message.to_shared_const_buffer ().size ();
	auto const window = window_size * size;
	auto const refill = refill_size * size;

	// The first thread sets the limit and counts the whole process, the others start sending once it is done
	std::optional<nano::benchmarks::process_allocation_tracker> tracker;
	nano::benchmarks::allocation_stats process_start;
	if (thread == 0)
	{
		fixture.node->outbound_limiter.reset (outbound_limit (options.limit_mib), type);
		tracker.emplace ();
		process_start = nano::benchmarks::process_allocations ();
	}

	std::atomic<uint64_t> completed{ 0 };
	auto const callback = [&completed] (auto const &, auto const &...) {
		completed.fetch_add (1, std::memory_order_relaxed);
	};

	uint64_t position = peer.received (); // Stream offset at the end of the last accepted send, the node's own messages to the peer move it too
	uint64_t accepted = 0;
	uint64_t dropped = 0;
	std::chrono::steady_clock::duration send_time{ 0 };
	auto const reads_start = peer.reads ();
	auto const sender_start = nano::benchmarks::thread_allocations ();

	for (auto _ : state)
	{
		for (size_t i = 0; i < burst_size; ++i)
		{
			// With the window full, waits until the peer has read all but `refill_size` messages of it
			if (position + size > peer.received () + window)
			{
				peer.wait_received (position - refill);
			}

			auto const start = std::chrono::steady_clock::now ();
			bool const sent = options.callbacks ? channel.send (message, type, callback) : channel.send (message, type);
			send_time += std::chrono::steady_clock::now () - start;

			if (sent)
			{
				position += size;
				++accepted;
			}
			else
			{
				++dropped;
			}
		}

		peer.wait_received (position);

		auto const expected_callbacks = options.callbacks ? accepted : 0;
		while (completed.load () < expected_callbacks)
		{
			std::this_thread::yield ();
		}
	}

	// Taken first, the other threads are done sending but may still be setting their counters
	auto const process = thread == 0 ? nano::benchmarks::process_allocations () - process_start : nano::benchmarks::allocation_stats{};
	tracker.reset ();
	auto const sender = nano::benchmarks::thread_allocations () - sender_start;
	auto const reads = peer.reads () - reads_start;

	auto const threads = static_cast<double> (state.threads ());
	auto const messages = static_cast<double> (state.iterations () * burst_size);
	// Rates are divided by the summed up time of all threads, scaling by the thread count turns that into the total throughput
	state.counters["msgs/s"] = benchmark::Counter (messages * threads, benchmark::Counter::kIsRate);
	state.counters["bytes/s"] = benchmark::Counter (messages * size * threads, benchmark::Counter::kIsRate, benchmark::Counter::kIs1024);
	state.counters["send_ns"] = benchmark::Counter (std::chrono::duration<double, std::nano> (send_time).count () / messages, benchmark::Counter::kAvgThreads);
	state.counters["send_allocs"] = benchmark::Counter (sender.count / messages, benchmark::Counter::kAvgThreads);
	state.counters["msgs/read"] = benchmark::Counter (reads != 0 ? accepted / static_cast<double> (reads) : 0, benchmark::Counter::kAvgThreads);
	state.counters["drops"] = benchmark::Counter (static_cast<double> (dropped));
	if (thread == 0)
	{
		state.counters["allocs/msg"] = benchmark::Counter (process.count / (messages * threads));
		state.counters["bytes/msg"] = benchmark::Counter (process.bytes / (messages * threads));
	}
}
}

// Keepalives, the smallest message a channel sends
static void BM_tcp_channel_keepalive (benchmark::State & state)
{
	auto & fixture = shared_fixture ();
	nano::messages::keepalive const message{ fixture.node->network_params.network };
	send_bursts (state, message, nano::transport::traffic_type::keepalive, { .callbacks = state.range (0) != 0, .limit_mib = static_cast<size_t> (state.range (1)) });
}

// Votes on `hashes` hashes, from the smallest confirm_ack to the largest
static void BM_tcp_channel_vote (benchmark::State & state)
{
	auto & fixture = shared_fixture ();
	nano::messages::confirm_ack const message{ fixture.node->network_params.network, fixture.vote (state.range (0)) };
	send_bursts (state, message, nano::transport::traffic_type::vote, { .callbacks = state.range (1) != 0, .limit_mib = static_cast<size_t> (state.range (2)) });
}

BENCHMARK (BM_tcp_channel_keepalive)->ArgNames ({ "callback", "limit_mib" })->ArgsProduct ({ { 0, 1 }, { 0, 2 } })->Threads (1)->Threads (channel_count)->UseRealTime ()->MeasureProcessCPUTime ();
BENCHMARK (BM_tcp_channel_vote)->ArgNames ({ "hashes", "callback", "limit_mib" })->ArgsProduct ({ { 1 }, { 0, 1 }, { 0, 2 } })->ArgsProduct ({ { 255 }, { 0, 1 }, { 0, 64 } })->Threads (1)->Threads (channel_count)->UseRealTime ()->MeasureProcessCPUTime ();
