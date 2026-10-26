#include <nano/lib/stats.hpp>
#include <nano/messages/keepalive.hpp>
#include <nano/node/message_processor.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/fake.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

/*
 * The messages of one channel are processed in the order they were handed over, whichever threads the processor runs.
 * Every channel sends keepalives numbered in a peer port, interleaved with the other channels as the io threads interleave them, into a processor that has not started yet, so its threads start into full queues as they do in a storm.
 * Once all of them are processed, the keepalive a channel is left with has to be the last one it sent.
 */
TEST (message_processor, channel_order)
{
	constexpr size_t channel_count = 64;
	constexpr size_t messages_per_channel = 1000;

	nano::test::system system;
	auto & node = *system.add_node ();

	// Enough threads to race, and room for every message so none is dropped
	nano::message_processor_config config;
	config.threads = 4;
	config.max_queue = messages_per_channel;
	nano::message_processor processor{ config, node };

	std::vector<std::shared_ptr<nano::transport::fake::channel>> channels;
	for (size_t i = 0; i < channel_count; ++i)
	{
		channels.push_back (std::make_shared<nano::transport::fake::channel> (node));
	}

	// Hand over one keepalive per channel in turn, numbered from 1 in the port of the second peer, the first peer is the self report
	for (size_t seq = 1; seq <= messages_per_channel; ++seq)
	{
		for (auto const & channel : channels)
		{
			auto message = std::make_unique<nano::messages::keepalive> (node.network_params.network);
			message->peers[1] = nano::endpoint{ boost::asio::ip::address_v6::any (), static_cast<uint16_t> (seq) };
			ASSERT_TRUE (processor.put (std::move (message), channel));
		}
	}

	// Start into the full queues and wait until every keepalive went through
	processor.start ();
	ASSERT_TIMELY_EQ (10s, node.stats.count (nano::stat::type::message, nano::stat::detail::keepalive, nano::stat::dir::in), channel_count * messages_per_channel);
	processor.stop ();

	// The keepalive a channel is left with is the last one it sent, an earlier one processed after it would have overwritten it
	for (auto const & channel : channels)
	{
		auto const last = channel->pop_last_keepalive ();
		ASSERT_TRUE (last.has_value ());
		ASSERT_EQ (messages_per_channel, last->peers[1].port ());
	}
}

/*
 * A channel's queue holds `max_queue` messages and drops the ones beyond, counting them, while another channel of the same thread still gets its messages in.
 */
TEST (message_processor, channel_overfill)
{
	constexpr size_t max_queue = 8;

	nano::test::system system;
	auto & node = *system.add_node ();

	// One thread, so both channels share a shard and the cap can only be the channel's own
	nano::message_processor_config config;
	config.threads = 1;
	config.max_queue = max_queue;
	nano::message_processor processor{ config, node };

	auto const full = std::make_shared<nano::transport::fake::channel> (node);
	auto const other = std::make_shared<nano::transport::fake::channel> (node);
	auto const keepalive = [&node] () {
		return std::make_unique<nano::messages::keepalive> (node.network_params.network);
	};

	// The channel takes exactly max_queue messages while the processor is stopped
	for (size_t i = 0; i < max_queue; ++i)
	{
		ASSERT_TRUE (processor.put (keepalive (), full));
	}
	ASSERT_EQ (0, node.stats.count (nano::stat::type::message_processor, nano::stat::detail::overfill, nano::stat::dir::in));

	// One more is dropped and counted, overall and by message type
	ASSERT_FALSE (processor.put (keepalive (), full));
	ASSERT_EQ (1, node.stats.count (nano::stat::type::message_processor, nano::stat::detail::overfill, nano::stat::dir::in));
	ASSERT_EQ (1, node.stats.count (nano::stat::type::message_processor_overfill, nano::stat::detail::keepalive, nano::stat::dir::in));

	// The other channel is not held back by the full one
	ASSERT_TRUE (processor.put (keepalive (), other));

	// Only what was queued gets processed, the dropped message is gone
	processor.start ();
	ASSERT_TIMELY_EQ (5s, node.stats.count (nano::stat::type::message, nano::stat::detail::keepalive, nano::stat::dir::in), max_queue + 1);
	processor.stop ();
	ASSERT_TRUE (other->pop_last_keepalive ().has_value ());
}
