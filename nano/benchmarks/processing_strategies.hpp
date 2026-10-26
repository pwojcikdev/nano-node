#pragma once

#include <nano/node/fair_queue.hpp>
#include <nano/secure/common.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nano::benchmarks
{
/*
 * Models of how a node could queue the messages its peers send and hand them to the threads that process them.
 * Every model takes a message with the channel it arrived on, holds at most `capacity` messages per queue and drops the rest, and calls one handler for every message it delivers.
 * They differ in what runs in parallel, and with it in the order the handler sees the messages of one channel:
 * - inline_strategy: no queue, the delivering thread processes the message itself
 * - fair_batch_strategy: the message processor before it was sharded, one fair queue over channels drained in batches by a pool of threads, which reorders the messages of a channel
 * - per_class_strategy: one fair queue and one thread per message class, keeps the order of each class on a channel
 * - sharded_strategy: fair queues with one thread each, a key always lands in the same one, keeps the order of a key; keyed by channel this is the message processor of today
 * - keyed_strategy: one pool of threads over per-key queues, one thread at a time drains a key, keeps the order of a key
 */

// Kind of message as the receiving side can tell it apart, from the message type; the traffic type a sender queued it under is not on the wire
enum class message_class : uint8_t
{
	vote, // confirm_ack
	block, // publish
	request, // confirm_req
	bootstrap, // asc_pull_req, asc_pull_ack
	relay, // vote_relay_req, vote_relay_ack
	misc, // keepalive, telemetry_ack
};
constexpr size_t message_class_count = 6;

// Stands in for a channel: one heap object per peer, held through shared_ptr as the real queues hold channels
struct channel_stub final
{
	uint32_t index; // Position among the channels of a run
};

// Stands in for a parsed message, heap allocated as the real ones are
struct bench_message final
{
	uint32_t channel; // Index of the channel it arrived on
	message_class type; // Kind of message
	uint64_t seq; // Position among the messages of its class on its channel, the order they were sent in
	uint64_t index; // Position among all messages of a run
	std::chrono::steady_clock::time_point enqueued; // When the delivering thread handed it over
};

// What the queues carry: a message and the channel it arrived on, as the message processor's entries do
using message_entry = std::pair<std::unique_ptr<bench_message>, std::shared_ptr<channel_stub>>;
// Called for every delivered message, on a thread of the strategy
using message_handler = std::function<void (bench_message const &)>;

// What the sharded and keyed strategies keep the order within
enum class order_key
{
	channel, // all messages of a channel are processed one after another
	channel_class, // messages of one class on a channel are, different classes may interleave
};

// Key of a message under the given order, mixed as a hash of the channel would be
uint64_t make_key (order_key, bench_message const &);

// A strategy is created stopped: what is handed over before `start` waits in its queues
class processing_strategy
{
public:
	virtual ~processing_strategy () = default;

	// Starts the processing threads
	virtual void start () = 0;
	// Stops and joins the processing threads, what is queued stays queued
	virtual void stop () = 0;

	// Hands a message over for processing, false if the queue of its origin is full and it was dropped
	virtual bool put (std::unique_ptr<bench_message>, std::shared_ptr<channel_stub> const &) = 0;
};

// No queue, the delivering thread processes the message itself
class inline_strategy final : public processing_strategy
{
public:
	explicit inline_strategy (message_handler);

	void start () override;
	void stop () override;
	bool put (std::unique_ptr<bench_message>, std::shared_ptr<channel_stub> const &) override;

private:
	message_handler handler;
};

/*
 * The message processor before it was sharded: one fair queue over channels, drained in batches by a pool of threads.
 * Two threads may hold messages of the same channel in their batches, so a channel's messages are processed out of order.
 */
class fair_batch_strategy final : public processing_strategy
{
public:
	fair_batch_strategy (message_handler, size_t threads, size_t batch_size, size_t capacity);
	~fair_batch_strategy () override;

	void start () override;
	void stop () override;
	bool put (std::unique_ptr<bench_message>, std::shared_ptr<channel_stub> const &) override;

private:
	void run ();

	message_handler handler;
	size_t const thread_count;
	size_t const batch_size; // Messages a thread takes out of the queue at once
	nano::fair_queue<message_entry, nano::no_value, std::shared_ptr<channel_stub>> queue;

	bool stopped{ true };
	std::mutex mutex;
	std::condition_variable condition;
	std::vector<std::thread> threads;
};

// A fair-queue processor with a single thread per message class, so a class never waits for another and a channel's messages of one class stay in order
class per_class_strategy final : public processing_strategy
{
public:
	per_class_strategy (message_handler, size_t batch_size, size_t capacity);

	void start () override;
	void stop () override;
	bool put (std::unique_ptr<bench_message>, std::shared_ptr<channel_stub> const &) override;

private:
	std::vector<std::unique_ptr<fair_batch_strategy>> lanes; // By message class
};

// Fair-queue processors with a single thread each, a key always lands in the same one by hash, so the messages of a key stay in order
class sharded_strategy final : public processing_strategy
{
public:
	sharded_strategy (message_handler, size_t shards, order_key, size_t batch_size, size_t capacity);

	void start () override;
	void stop () override;
	bool put (std::unique_ptr<bench_message>, std::shared_ptr<channel_stub> const &) override;

private:
	order_key const key; // What a shard is picked by
	std::vector<std::unique_ptr<fair_batch_strategy>> shards;
};

/*
 * One pool of threads over per-key queues: a key is drained by one thread at a time and the keys with messages take turns.
 * Any thread may process any key, but the messages of a key stay in order.
 */
class keyed_strategy final : public processing_strategy
{
public:
	keyed_strategy (message_handler, size_t threads, order_key, size_t lane_batch, size_t capacity);
	~keyed_strategy () override;

	void start () override;
	void stop () override;
	bool put (std::unique_ptr<bench_message>, std::shared_ptr<channel_stub> const &) override;

private:
	// Queue of one key
	struct lane
	{
		std::deque<message_entry> queue;
		bool active{ false }; // A thread is processing messages taken from it
		bool ready{ false }; // Listed in `ready`
	};

	void run ();

	message_handler handler;
	size_t const thread_count;
	order_key const key; // What a lane is picked by
	size_t const lane_batch; // Messages a thread takes from a lane per turn, the share of a key in a round
	size_t const capacity; // Messages a lane holds before it drops

	std::unordered_map<uint64_t, lane> lanes; // By key, node based so a lane stays put while listed in `ready`
	std::deque<lane *> ready; // Lanes with messages that no thread is draining, in the order they became ready

	bool stopped{ true };
	std::mutex mutex;
	std::condition_variable condition;
	std::vector<std::thread> threads;
};
}
