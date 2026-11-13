#include <nano/lib/rate_limiting.hpp>
#include <nano/messages/confirm.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/tcp_channel.hpp>
#include <nano/node/transport/tcp_socket.hpp>
#include <nano/test_common/peer.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

using namespace std::chrono_literals;

namespace
{
// A vote large enough that a channel's bandwidth allocation covers only a couple of dozen of them
nano::messages::confirm_ack make_large_vote ()
{
	std::vector<nano::block_hash> hashes;
	for (uint64_t i = 1; i <= 200; ++i)
	{
		hashes.emplace_back (i);
	}
	return nano::messages::confirm_ack{ nano::dev::network_params.network, nano::test::make_vote (nano::dev::genesis_key, hashes) };
}

// Tallies the outcomes reported to send callbacks
struct send_outcomes
{
	std::atomic<size_t> written{ 0 };
	std::atomic<size_t> aborted{ 0 };
	std::atomic<size_t> failed{ 0 };

	nano::transport::send_callback callback ()
	{
		return [this] (boost::system::error_code const & ec, std::size_t size) {
			if (!ec)
			{
				++written;
			}
			else if (ec == boost::asio::error::operation_aborted)
			{
				++aborted;
			}
			else
			{
				++failed;
			}
		};
	}

	size_t total () const
	{
		return written + aborted + failed;
	}
};
}

/*
 * Closing a channel whose sending stalled on bandwidth fails every send it has not written: each callback runs once, the unwritten ones
 * with operation_aborted, and each of those counts as a drop of a closed channel. A send after the close is refused and counted the same way
 */
TEST (tcp_channel, close_fails_unsent)
{
	nano::test::system system;
	auto & node = *system.add_node ();
	nano::test::peer peer{ node };
	ASSERT_TRUE (peer.handshake ());
	auto const channel = node.network.find_node_id (peer.node_id ());
	ASSERT_NE (nullptr, channel);

	// Starved of outbound bandwidth, the channel writes what its allocation covers and then stalls with the rest queued
	node.outbound_limiter.reset (nano::rate_limit{ 1, 1.0 });
	auto const large = make_large_vote ();
	send_outcomes outcomes;
	size_t const count = 64;
	for (size_t i = 0; i < count; ++i)
	{
		ASSERT_TRUE (channel->send (large, nano::transport::traffic_type::test, outcomes.callback ()));
	}
	ASSERT_TIMELY (5s, node.stats.count (nano::stat::type::tcp_channel_wait, nano::stat::detail::wait_bandwidth, nano::stat::dir::out) >= 1);

	// Closing the channel reports every send, the stalled ones as aborted
	channel->close ();
	ASSERT_TIMELY_EQ (5s, count, outcomes.total ());
	ASSERT_LT (0, outcomes.aborted.load ());
	ASSERT_EQ (0, outcomes.failed.load ());
	ASSERT_EQ (outcomes.aborted.load (), node.stats.count (nano::stat::type::tcp_channel, nano::stat::detail::drop_closed, nano::stat::dir::out));

	// A send after the close is refused and counted, and its callback never runs
	ASSERT_FALSE (channel->send (large, nano::transport::traffic_type::test, outcomes.callback ()));
	ASSERT_EQ (outcomes.aborted.load () + 1, node.stats.count (nano::stat::type::tcp_channel, nano::stat::detail::drop_closed, nano::stat::dir::out));
	ASSERT_EQ (count, outcomes.total ());
}

/*
 * When a write fails, the sends behind it are failed too instead of staying queued with callbacks that never run.
 * The socket is closed under a channel stalled on bandwidth, then the bandwidth returns, so the next write meets the closed socket
 */
TEST (tcp_channel, write_error_fails_unsent)
{
	nano::test::system system;
	auto & node = *system.add_node ();
	nano::test::peer peer{ node };
	ASSERT_TRUE (peer.handshake ());
	auto const channel = std::dynamic_pointer_cast<nano::transport::tcp_channel> (node.network.find_node_id (peer.node_id ()));
	ASSERT_NE (nullptr, channel);

	// A stalled channel with sends queued behind the stall
	node.outbound_limiter.reset (nano::rate_limit{ 1, 1.0 });
	auto const large = make_large_vote ();
	send_outcomes outcomes;
	size_t const count = 64;
	for (size_t i = 0; i < count; ++i)
	{
		ASSERT_TRUE (channel->send (large, nano::transport::traffic_type::test, outcomes.callback ()));
	}
	ASSERT_TIMELY (5s, node.stats.count (nano::stat::type::tcp_channel_wait, nano::stat::detail::wait_bandwidth, nano::stat::dir::out) >= 1);
	auto const written_before = outcomes.written.load ();

	// The socket goes away while the channel waits, then bandwidth returns
	channel->socket->close ();
	node.outbound_limiter.reset (nano::rate_limit::unlimited ());

	// Every send is reported, none of the remaining ones as written
	ASSERT_TIMELY_EQ (5s, count, outcomes.total ());
	ASSERT_EQ (written_before, outcomes.written.load ());
	ASSERT_LT (0, outcomes.aborted.load ());
	ASSERT_EQ (outcomes.aborted.load (), node.stats.count (nano::stat::type::tcp_channel, nano::stat::detail::drop_closed, nano::stat::dir::out));
}
