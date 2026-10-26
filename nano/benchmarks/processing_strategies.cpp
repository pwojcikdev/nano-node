#include <nano/benchmarks/processing_strategies.hpp>
#include <nano/lib/utility.hpp>

#include <algorithm>

namespace
{
// Final mixing step of splitmix64, spreads neighbouring keys over the whole range as a pointer hash would
uint64_t mix (uint64_t x)
{
	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;
	return x;
}
}

uint64_t nano::benchmarks::make_key (order_key key, bench_message const & message)
{
	uint64_t const channel = message.channel;
	uint64_t const type = key == order_key::channel_class ? static_cast<uint64_t> (message.type) : 0;
	return mix ((channel << 8) | type);
}

/*
 * inline_strategy
 */

nano::benchmarks::inline_strategy::inline_strategy (message_handler handler_a) :
	handler{ std::move (handler_a) }
{
}

void nano::benchmarks::inline_strategy::start ()
{
}

void nano::benchmarks::inline_strategy::stop ()
{
}

bool nano::benchmarks::inline_strategy::put (std::unique_ptr<bench_message> message, std::shared_ptr<channel_stub> const &)
{
	handler (*message);
	return true;
}

/*
 * fair_batch_strategy
 */

nano::benchmarks::fair_batch_strategy::fair_batch_strategy (message_handler handler_a, size_t threads_a, size_t batch_size_a, size_t capacity) :
	handler{ std::move (handler_a) },
	thread_count{ threads_a },
	batch_size{ batch_size_a }
{
	queue.max_size_query = [capacity] (auto const &) {
		return capacity;
	};
	// The message processor gives every channel one message per round
	queue.priority_query = [] (auto const &) {
		return 1;
	};
}

nano::benchmarks::fair_batch_strategy::~fair_batch_strategy ()
{
	stop ();
}

void nano::benchmarks::fair_batch_strategy::start ()
{
	release_assert (threads.empty ());
	{
		std::lock_guard guard{ mutex };
		stopped = false;
	}
	for (size_t i = 0; i < thread_count; ++i)
	{
		threads.emplace_back ([this] {
			run ();
		});
	}
}

void nano::benchmarks::fair_batch_strategy::stop ()
{
	{
		std::lock_guard guard{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	for (auto & thread : threads)
	{
		thread.join ();
	}
	threads.clear ();
}

bool nano::benchmarks::fair_batch_strategy::put (std::unique_ptr<bench_message> message, std::shared_ptr<channel_stub> const & channel)
{
	bool added = false;
	{
		std::lock_guard guard{ mutex };
		added = queue.push ({ std::move (message), channel }, { nano::no_value{}, channel });
	}
	if (added)
	{
		// As message_processor::put does, every thread wakes up
		condition.notify_all ();
	}
	return added;
}

void nano::benchmarks::fair_batch_strategy::run ()
{
	std::unique_lock lock{ mutex };
	while (!stopped)
	{
		if (!queue.empty ())
		{
			auto batch = queue.next_batch (batch_size);
			lock.unlock ();
			for (auto const & [entry, origin] : batch)
			{
				handler (*entry.first);
			}
			lock.lock ();
		}
		else
		{
			condition.wait (lock, [this] {
				return stopped || !queue.empty ();
			});
		}
	}
}

/*
 * per_class_strategy
 */

nano::benchmarks::per_class_strategy::per_class_strategy (message_handler handler, size_t batch_size, size_t capacity)
{
	for (size_t i = 0; i < message_class_count; ++i)
	{
		lanes.push_back (std::make_unique<fair_batch_strategy> (handler, 1, batch_size, capacity));
	}
}

void nano::benchmarks::per_class_strategy::start ()
{
	for (auto & lane : lanes)
	{
		lane->start ();
	}
}

void nano::benchmarks::per_class_strategy::stop ()
{
	for (auto & lane : lanes)
	{
		lane->stop ();
	}
}

bool nano::benchmarks::per_class_strategy::put (std::unique_ptr<bench_message> message, std::shared_ptr<channel_stub> const & channel)
{
	auto & lane = *lanes[static_cast<size_t> (message->type)];
	return lane.put (std::move (message), channel);
}

/*
 * sharded_strategy
 */

nano::benchmarks::sharded_strategy::sharded_strategy (message_handler handler, size_t shards_a, order_key key_a, size_t batch_size, size_t capacity) :
	key{ key_a }
{
	for (size_t i = 0; i < shards_a; ++i)
	{
		shards.push_back (std::make_unique<fair_batch_strategy> (handler, 1, batch_size, capacity));
	}
}

void nano::benchmarks::sharded_strategy::start ()
{
	for (auto & shard : shards)
	{
		shard->start ();
	}
}

void nano::benchmarks::sharded_strategy::stop ()
{
	for (auto & shard : shards)
	{
		shard->stop ();
	}
}

bool nano::benchmarks::sharded_strategy::put (std::unique_ptr<bench_message> message, std::shared_ptr<channel_stub> const & channel)
{
	auto & shard = *shards[make_key (key, *message) % shards.size ()];
	return shard.put (std::move (message), channel);
}

/*
 * keyed_strategy
 */

nano::benchmarks::keyed_strategy::keyed_strategy (message_handler handler_a, size_t threads_a, order_key key_a, size_t lane_batch_a, size_t capacity_a) :
	handler{ std::move (handler_a) },
	thread_count{ threads_a },
	key{ key_a },
	lane_batch{ lane_batch_a },
	capacity{ capacity_a }
{
}

nano::benchmarks::keyed_strategy::~keyed_strategy ()
{
	stop ();
}

void nano::benchmarks::keyed_strategy::start ()
{
	release_assert (threads.empty ());
	{
		std::lock_guard guard{ mutex };
		stopped = false;
	}
	for (size_t i = 0; i < thread_count; ++i)
	{
		threads.emplace_back ([this] {
			run ();
		});
	}
}

void nano::benchmarks::keyed_strategy::stop ()
{
	{
		std::lock_guard guard{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	for (auto & thread : threads)
	{
		thread.join ();
	}
	threads.clear ();
}

bool nano::benchmarks::keyed_strategy::put (std::unique_ptr<bench_message> message, std::shared_ptr<channel_stub> const & channel)
{
	auto const key_l = make_key (key, *message);
	bool added = false;
	bool wake = false;
	{
		std::lock_guard guard{ mutex };
		auto & lane_l = lanes[key_l];
		if (lane_l.queue.size () < capacity)
		{
			lane_l.queue.emplace_back (std::move (message), channel);
			added = true;
			// A lane a thread is draining comes back to the ready list once that thread is done with it
			if (!lane_l.active && !lane_l.ready)
			{
				ready.push_back (&lane_l);
				lane_l.ready = true;
				wake = true;
			}
		}
	}
	if (wake)
	{
		condition.notify_one ();
	}
	return added;
}

void nano::benchmarks::keyed_strategy::run ()
{
	std::vector<message_entry> batch;
	batch.reserve (lane_batch);

	std::unique_lock lock{ mutex };
	while (!stopped)
	{
		if (ready.empty ())
		{
			condition.wait (lock, [this] {
				return stopped || !ready.empty ();
			});
			continue;
		}

		auto & lane_l = *ready.front ();
		ready.pop_front ();
		lane_l.ready = false;
		lane_l.active = true;

		auto const count = std::min (lane_batch, lane_l.queue.size ());
		for (size_t i = 0; i < count; ++i)
		{
			batch.push_back (std::move (lane_l.queue.front ()));
			lane_l.queue.pop_front ();
		}
		lock.unlock ();

		for (auto const & entry : batch)
		{
			handler (*entry.first);
		}
		batch.clear ();

		lock.lock ();
		lane_l.active = false;
		// Whatever arrived meanwhile waits for the lane's next turn
		if (!lane_l.queue.empty ())
		{
			ready.push_back (&lane_l);
			lane_l.ready = true;
		}
	}
}
