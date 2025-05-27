#include <nano/node/transport/message_queue.hpp>

#include <random>

#include <benchmark/benchmark.h>

using namespace nano::transport;

namespace
{
// Re‑use one tiny dummy buffer for every message we enqueue – we only care
// about the queue mechanics, not buffer allocation.
nano::shared_const_buffer make_dummy_buffer ()
{
	static nano::shared_const_buffer buf{ 42 };
	return buf;
}

message_queue::callback_t make_dummy_callback ()
{
	static message_queue::callback_t cb = [] (boost::system::error_code const &, std::size_t) {
		// Do nothing
	};
	return cb;
}

message_queue::entry_t make_entry ()
{
	return { make_dummy_buffer (), make_dummy_callback () };
}
}

/*
 * Push N messages of the same traffic type
 */
static void BM_message_queue_push_single_traffic (benchmark::State & state)
{
	const auto type = traffic_type::generic;
	const auto entry = make_entry ();

	message_queue q;
	for (auto _ : state)
	{
		state.PauseTiming ();
		q.clear ();
		state.ResumeTiming ();

		for (int i = 0; i < state.range (0); ++i)
		{
			if (!q.full (type))
			{
				q.push (type, entry);
			}
		}
		benchmark::DoNotOptimize (q);
	}
}
BENCHMARK (BM_message_queue_push_single_traffic)
->RangeMultiplier (10)
->Range (1, nano::transport::message_queue::full_size);

/*
 * Push N messages of random traffic types, chosen uniformly at random.
 */
static void BM_message_queue_push_random_traffic (benchmark::State & state)
{
	auto types = all_traffic_types ();
	std::mt19937 rng (0xC0FFEE);
	std::uniform_int_distribution<std::size_t> pick (0, types.size () - 1);
	const auto entry = make_entry ();

	message_queue q;
	for (auto _ : state)
	{
		state.PauseTiming ();
		q.clear ();
		state.ResumeTiming ();

		for (int i = 0; i < state.range (0); ++i)
		{
			auto t = types[pick (rng)];
			if (!q.full (t))
			{
				q.push (t, entry);
			}
		}
		benchmark::DoNotOptimize (q);
	}
}
BENCHMARK (BM_message_queue_push_random_traffic)
->RangeMultiplier (10)
->Range (1, nano::transport::message_queue::full_size * 2);

/*
 * Fill the queue, then drain it with repeated next() calls
 */
static void BM_message_queue_next (benchmark::State & state)
{
	auto types = all_traffic_types ();
	const auto entry = make_entry ();

	message_queue q;
	for (auto _ : state)
	{
		state.PauseTiming ();

		q.clear ();
		for (int i = 0; i < state.range (0); ++i)
		{
			q.push (types[i % types.size ()], entry);
		}

		state.ResumeTiming ();

		while (!q.empty ())
		{
			benchmark::DoNotOptimize (q.next ());
		}
		benchmark::ClobberMemory ();
	}
}
BENCHMARK (BM_message_queue_next)
->RangeMultiplier (10)
->Range (1, nano::transport::message_queue::full_size);

/*
 * Fill the queue, then drain it with repeated next_batch() calls
 */
static void BM_message_queue_next_batch (benchmark::State & state)
{
	auto types = all_traffic_types ();
	const auto entry = make_entry ();

	message_queue q;
	for (auto _ : state)
	{
		state.PauseTiming ();

		q.clear ();
		for (std::size_t i = 0; i < state.range (0); ++i)
		{
			q.push (types[i % types.size ()], entry);
		}

		state.ResumeTiming ();

		while (!q.empty ())
		{
			benchmark::DoNotOptimize (q.next_batch ());
		}
		benchmark::ClobberMemory ();
	}
}
BENCHMARK (BM_message_queue_next_batch)
->RangeMultiplier (10)
->Range (1, nano::transport::message_queue::full_size * 4);