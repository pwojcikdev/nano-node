#pragma once

#include <nano/lib/interval.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/threading.hpp>
#include <nano/node/fair_queue_traits.hpp>
#include <nano/node/fwd.hpp>

#include <deque>
#include <thread>

namespace nano
{
class message_processor_config final
{
public:
	nano::error deserialize (nano::tomlconfig & toml);
	nano::error serialize (nano::tomlconfig & toml) const;

public:
	size_t threads{ std::clamp (nano::hardware_concurrency () / 4, 1u, 2u) }; // Processing threads, each owning a share of the channels
	size_t max_queue{ 64 }; // Messages queued per channel before further ones are dropped
};

/*
 * Queues the messages peers send and processes them on a pool of threads.
 * Every thread owns a shard with a fair queue of its own, and a channel always lands in the same shard, so the messages of one channel are processed one after another in the order they arrived.
 */
class message_processor final
{
public:
	explicit message_processor (message_processor_config const &, nano::node &);
	~message_processor ();

	void start ();
	void stop ();

	// Queues a message from the channel for processing, false if the channel's queue is full and it was dropped
	bool put (std::unique_ptr<nano::messages::message>, std::shared_ptr<nano::transport::channel> const &);
	// Hands a message to the component that handles its type, on the calling thread
	void process (nano::messages::message const &, std::shared_ptr<nano::transport::channel> const &);

	nano::container_info container_info () const;

private:
	using entry_t = std::pair<std::unique_ptr<nano::messages::message>, std::shared_ptr<nano::transport::channel>>;

	// The queue of one thread's channels and what runs it
	struct shard final
	{
		nano::fair_queue<entry_t, nano::no_value, std::shared_ptr<nano::transport::channel>> queue;
		mutable nano::mutex mutex;
		nano::condition_variable condition;
		std::thread thread;
		nano::interval log_interval;
	};

	// The shard a channel's messages go to, the same one for the channel's lifetime
	shard & shard_for (std::shared_ptr<nano::transport::channel> const &);
	void run (shard &);
	void run_batch (shard &, nano::unique_lock<nano::mutex> &);

private: // Dependencies
	message_processor_config const & config;
	nano::node & node;
	nano::stats & stats;
	nano::logger & logger;

private:
	std::deque<shard> shards; // One per configured thread, a deque since a shard holds a mutex and must never move
	std::atomic<bool> stopped{ false };
};
}
