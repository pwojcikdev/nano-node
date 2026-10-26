/*
 * Benchmarks of the queuing strategies in processing_strategies.hpp under a synthetic load, standing in for the message processor and the io threads that feed it.
 * `producer_count` threads hand messages over on the channels of a run, a few messages of one channel in a row as a peer sends them, the strategy's threads process them, and processing a message costs a fixed spin per message class.
 *
 * Families:
 * - burst: every channel sends `lane_capacity` messages while the strategy is stopped, as a vote storm fills the queues, then the strategy starts and the drain is timed. The next burst follows once the queues are empty. Measures how fast full queues drain, in what order, and what a hand-over costs the delivering thread
 * - paced: messages arrive at a steady offered rate for `paced_duration`, below and above what one thread processes. Measures latency, drops and reordering under load
 * - hot: paced at `hot_rate`, with half of all messages from one channel. Shows what a strategy does to a peer that floods it and to the peers that do not
 *
 * Work profiles:
 * - light: every message costs 500 ns, about what handing a message to the component that processes it costs
 * - mixed: as light, but a bootstrap message costs 10 µs and a misc message 50 µs, the signature check of a telemetry ack, which today's handlers do inline
 *
 * Every run reports items/s processed, the queue latency from hand-over (or the start of a drain) to the start of processing as p50, p99 and max in µs, drop_pct, and reorder_pct: messages processed after a later message of the same class on the same channel had been.
 * offered_per_s is the rate the producers reached, which falls short of the asked one when the strategy slows them down. A burst run adds put_ns, the time a hand-over takes the delivering thread.
 * The hot family reports latency and drops of the hot channel and of the others apart.
 * A thread count is the pool of fair_batch and keyed and the number of shards of sharded; inline runs on the delivering threads and per_class has one thread per class.
 *
 * Record a run with --benchmark_out=<file> --benchmark_out_format=json and compare two of them with submodules/benchmark/tools/compare.py
 */

#include <nano/benchmarks/processing_strategies.hpp>
#include <nano/lib/utility.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

using namespace std::chrono_literals;
using namespace nano::benchmarks;
using std::chrono::steady_clock;

namespace
{
constexpr size_t channel_count = 256; // About the peer count on the live network
constexpr size_t producer_count = 4; // Threads handing messages over, as the io threads do
constexpr size_t lane_capacity = 64; // Messages queued per channel, message_processor_config::max_queue
constexpr size_t production_batch = 4096; // Messages a thread takes at once, message_processor::run_batch
constexpr size_t lane_batch = 8; // Messages a keyed thread takes from one key per turn
constexpr size_t burst_rounds = 16;
constexpr std::chrono::milliseconds paced_duration{ 500 };
constexpr std::array<uint64_t, 3> paced_rates{ 100'000, 500'000, 2'000'000 };
constexpr uint64_t hot_rate = 500'000;
constexpr unsigned hot_percent = 50;
constexpr std::array<int64_t, 3> thread_counts{ 1, 2, 4 };

// Share of live traffic per message class, per mille
constexpr std::array<unsigned, message_class_count> traffic_mix{ 700, 150, 100, 40, 5, 5 };

// Work per message by class, in nanoseconds
using work_profile = std::array<uint32_t, message_class_count>;
constexpr work_profile light_profile{ 500, 500, 500, 500, 500, 500 };
constexpr work_profile mixed_profile{ 500, 500, 500, 10'000, 2'000, 50'000 };

// Burns `iterations` steps of an integer mix, the stand-in for a handler's work
void spin (uint64_t iterations)
{
	uint64_t x = 0x9e3779b97f4a7c15ULL;
	for (uint64_t i = 0; i < iterations; ++i)
	{
		x ^= x >> 12;
		x ^= x << 25;
		x ^= x >> 27;
		x *= 0x2545f4914f6cdd1dULL;
	}
	benchmark::DoNotOptimize (x);
}

// Iterations of spin that take one microsecond on this machine, measured once
uint64_t iterations_per_microsecond ()
{
	static uint64_t const result = [] {
		constexpr uint64_t probe = 1'000'000;
		std::array<double, 7> samples{};
		for (auto & sample : samples)
		{
			auto const start = steady_clock::now ();
			spin (probe);
			sample = std::chrono::duration<double, std::micro> (steady_clock::now () - start).count ();
		}
		std::sort (samples.begin (), samples.end ());
		return static_cast<uint64_t> (probe / samples[samples.size () / 2]);
	}();
	return result;
}

int64_t since_epoch (steady_clock::time_point time)
{
	return time.time_since_epoch ().count ();
}

/*
 * Receives the delivered messages: does the work of the message's class, records how long it waited and whether it was overtaken by a later message of its stream.
 * A stream is the messages of one class on one channel, the order they were sent in.
 */
class sink final
{
public:
	sink (size_t channels, size_t messages, work_profile const & profile) :
		latency_ns (messages, 0),
		streams (channels * message_class_count)
	{
		auto const per_us = iterations_per_microsecond ();
		for (size_t i = 0; i < message_class_count; ++i)
		{
			iterations[i] = profile[i] * per_us / 1000;
		}
	}

	// Latency counts from this moment for the messages handed over earlier, the start of a drain
	void set_baseline (steady_clock::time_point time)
	{
		baseline.store (since_epoch (time), std::memory_order_release);
	}

	void process (bench_message const & message)
	{
		auto const waited = since_epoch (steady_clock::now ()) - std::max (since_epoch (message.enqueued), baseline.load (std::memory_order_relaxed));
		latency_ns[message.index] = static_cast<uint32_t> (std::clamp<int64_t> (waited, 1, std::numeric_limits<uint32_t>::max ()));

		spin (iterations[static_cast<size_t> (message.type)]);

		// A message finishing after a later message of its stream has finished was overtaken
		auto & last = streams[message.channel * message_class_count + static_cast<size_t> (message.type)].completed;
		uint64_t const done = message.seq + 1;
		uint64_t previous = last.load (std::memory_order_relaxed);
		while (previous < done && !last.compare_exchange_weak (previous, done, std::memory_order_relaxed))
		{
		}
		if (previous > done)
		{
			reorders_m.fetch_add (1, std::memory_order_relaxed);
		}
		processed_m.fetch_add (1, std::memory_order_release);
	}

	uint64_t processed () const
	{
		return processed_m.load (std::memory_order_acquire);
	}

	uint64_t reorders () const
	{
		return reorders_m.load (std::memory_order_relaxed);
	}

	// Nanoseconds a message waited in the queue, by message index, 0 for a message that was never processed
	std::vector<uint32_t> const & latencies () const
	{
		return latency_ns;
	}

private:
	// Padded so that the threads processing different streams do not share a cache line
	struct alignas (64) stream_state
	{
		std::atomic<uint64_t> completed{ 0 }; // Highest seq finished so far, plus one
	};

	std::array<uint64_t, message_class_count> iterations{}; // Spin per message class
	std::atomic<int64_t> baseline{ 0 };
	std::vector<uint32_t> latency_ns;
	std::vector<stream_state> streams;
	std::atomic<uint64_t> processed_m{ 0 };
	std::atomic<uint64_t> reorders_m{ 0 };
};

/*
 * What a run offers. With a rate the producers pace themselves and the strategy runs throughout.
 * Without one, every round is handed over as fast as possible while the strategy is stopped, and the strategy then runs until the round is drained.
 */
struct workload
{
	size_t channels{ channel_count };
	size_t rounds{ 1 }; // Bursts, each drained before the next one starts
	size_t messages{ 0 }; // Messages per round over all channels
	uint64_t rate{ 0 }; // Messages per second offered over all producers, 0 for as fast as possible
	unsigned hot_percent{ 0 }; // Share of the messages that go to channel 0, 0 for an even spread
	size_t run_length{ 4 }; // Messages a channel sends in a row before its producer moves on, as a peer sends a few at a time
};

// One message a producer sends
struct pick
{
	uint32_t channel;
	message_class type;
};

// What one producer sends: its picks for all rounds in order, the same count every round
struct producer_plan
{
	std::vector<pick> picks;
	size_t per_round{ 0 };
	uint64_t first_index{ 0 }; // Index of its first message among all messages of the run
};

uint64_t xorshift (uint64_t & state)
{
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return state;
}

// Deals the messages of a round out to the producers, each walking its channels in turn, a run of messages per channel, so channels interleave as they do on the io threads
std::vector<producer_plan> make_plans (workload const & load)
{
	// Messages per channel and round
	std::vector<uint64_t> budget (load.channels, load.messages / load.channels);
	uint64_t hot_weight = 1;
	if (load.hot_percent > 0)
	{
		uint64_t const cold_each = load.messages * (100 - load.hot_percent) / 100 / (load.channels - 1);
		std::fill (budget.begin (), budget.end (), cold_each);
		budget[0] = load.messages - cold_each * (load.channels - 1);
		// Hot messages per message of a cold channel, so the hot channel's share is spread over the round
		hot_weight = (budget[0] + cold_each - 1) / cold_each;
	}

	// Class by a draw per message, the same sequence every run
	std::array<message_class, 1000> table{};
	size_t filled = 0;
	for (size_t type = 0; type < message_class_count; ++type)
	{
		std::fill_n (table.begin () + filled, traffic_mix[type], static_cast<message_class> (type));
		filled += traffic_mix[type];
	}
	release_assert (filled == table.size ());

	std::vector<producer_plan> plans (producer_count);
	uint64_t next_index = 0;
	for (size_t producer = 0; producer < producer_count; ++producer)
	{
		auto & plan = plans[producer];
		size_t const first = producer * load.channels / producer_count;
		size_t const last = (producer + 1) * load.channels / producer_count;

		std::vector<uint64_t> generators (load.channels);
		for (size_t channel = 0; channel < load.channels; ++channel)
		{
			generators[channel] = (channel + 1) * 0x9e3779b97f4a7c15ULL;
		}

		for (size_t round = 0; round < load.rounds; ++round)
		{
			std::vector<uint64_t> remaining (budget.begin (), budget.end ());
			bool more = true;
			while (more)
			{
				more = false;
				for (size_t channel = first; channel < last; ++channel)
				{
					auto const weight = (channel == 0 ? hot_weight : 1) * load.run_length;
					for (uint64_t i = 0; i < weight && remaining[channel] > 0; ++i)
					{
						plan.picks.push_back ({ static_cast<uint32_t> (channel), table[xorshift (generators[channel]) % table.size ()] });
						--remaining[channel];
					}
					more |= remaining[channel] > 0;
				}
			}
		}

		plan.per_round = plan.picks.size () / load.rounds;
		plan.first_index = next_index;
		next_index += plan.picks.size ();
	}
	return plans;
}

// Padded so that the producers do not share a cache line
struct alignas (64) producer_stats
{
	std::atomic<uint64_t> dropped_hot{ 0 }; // Messages of channel 0 the strategy refused
	std::atomic<uint64_t> dropped_cold{ 0 }; // Messages of the other channels it refused
	std::chrono::nanoseconds busy{ 0 }; // Time spent handing messages over, pacing included
};

// Lets the producers start together and holds them between rounds until the queues are drained
struct run_control
{
	std::atomic<bool> start{ false };
	std::atomic<size_t> finished{ 0 }; // Producer rounds finished so far
	std::atomic<size_t> released{ 0 }; // Rounds the producers may go past

	void wait_start () const
	{
		while (!start.load (std::memory_order_acquire))
		{
			std::this_thread::yield ();
		}
	}

	void round_finished (size_t round)
	{
		finished.fetch_add (1, std::memory_order_acq_rel);
		while (released.load (std::memory_order_acquire) <= round)
		{
			std::this_thread::yield ();
		}
	}
};

// Hands the plan's messages over, paced to the producer's share of the offered rate
void produce (processing_strategy & strategy, std::vector<std::shared_ptr<channel_stub>> const & channels, producer_plan const & plan, workload const & load, run_control & control, producer_stats & stats)
{
	std::vector<uint64_t> next_seq (load.channels * message_class_count, 0);
	std::chrono::nanoseconds interval{ 0 };
	if (load.rate > 0)
	{
		interval = std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::duration<double> (static_cast<double> (load.messages) / (static_cast<double> (load.rate) * plan.per_round)));
	}

	control.wait_start ();
	for (size_t round = 0; round < load.rounds; ++round)
	{
		auto const round_start = steady_clock::now ();
		for (size_t i = 0; i < plan.per_round; ++i)
		{
			auto const position = round * plan.per_round + i;
			auto const & pick_l = plan.picks[position];

			if (interval > 0ns)
			{
				auto const due = round_start + interval * i;
				while (steady_clock::now () < due)
				{
				}
			}

			auto message = std::make_unique<bench_message> ();
			message->channel = pick_l.channel;
			message->type = pick_l.type;
			message->seq = next_seq[pick_l.channel * message_class_count + static_cast<size_t> (pick_l.type)]++;
			message->index = plan.first_index + position;
			message->enqueued = steady_clock::now ();

			if (!strategy.put (std::move (message), channels[pick_l.channel]))
			{
				auto & dropped = pick_l.channel == 0 ? stats.dropped_hot : stats.dropped_cold;
				dropped.fetch_add (1, std::memory_order_relaxed);
			}
		}
		stats.busy += steady_clock::now () - round_start;
		control.round_finished (round);
	}
}

struct latency_summary
{
	double p50_us{ 0 };
	double p99_us{ 0 };
	double max_us{ 0 };
};

latency_summary summarize (std::vector<uint32_t> & values)
{
	latency_summary summary;
	if (values.empty ())
	{
		return summary;
	}
	auto const percentile = [&values] (size_t numerator, size_t denominator) {
		auto const position = std::min (values.size () - 1, values.size () * numerator / denominator);
		std::nth_element (values.begin (), values.begin () + position, values.end ());
		return values[position] / 1000.0;
	};
	summary.p50_us = percentile (50, 100);
	summary.p99_us = percentile (99, 100);
	summary.max_us = *std::max_element (values.begin (), values.end ()) / 1000.0;
	return summary;
}

struct scenario_result
{
	std::chrono::nanoseconds elapsed{ 0 }; // Time the strategy was processing, until the last message of a round was done
	std::chrono::nanoseconds busy{ 0 }; // Sum of the producers' hand-over time
	uint64_t offered{ 0 };
	uint64_t processed{ 0 };
	uint64_t dropped{ 0 };
	uint64_t dropped_hot{ 0 };
	uint64_t offered_hot{ 0 };
	uint64_t reorders{ 0 };
	latency_summary all;
	latency_summary hot;
	latency_summary cold;
};

using strategy_factory = std::function<std::unique_ptr<processing_strategy> (message_handler, size_t threads)>;

uint64_t dropped_total (std::vector<producer_stats> const & stats)
{
	uint64_t total = 0;
	for (auto const & producer : stats)
	{
		total += producer.dropped_hot.load (std::memory_order_relaxed) + producer.dropped_cold.load (std::memory_order_relaxed);
	}
	return total;
}

// Runs the workload once against a fresh strategy and gathers what happened to every message
scenario_result run_scenario (strategy_factory const & make, size_t threads, workload const & load, work_profile const & profile)
{
	auto const plans = make_plans (load);
	uint64_t total = 0;
	for (auto const & plan : plans)
	{
		total += plan.picks.size ();
	}

	sink target{ load.channels, total, profile };
	std::vector<std::shared_ptr<channel_stub>> channels;
	for (uint32_t i = 0; i < load.channels; ++i)
	{
		channels.push_back (std::make_shared<channel_stub> (channel_stub{ i }));
	}
	std::vector<producer_stats> stats (producer_count);
	run_control control;
	bool const paced = load.rate > 0;

	scenario_result result;
	{
		auto strategy = make ([&target] (bench_message const & message) {
			target.process (message);
		},
		threads);

		std::vector<std::thread> producers;
		for (size_t i = 0; i < producer_count; ++i)
		{
			producers.emplace_back (produce, std::ref (*strategy), std::cref (channels), std::cref (plans[i]), std::cref (load), std::ref (control), std::ref (stats[i]));
		}

		auto const begin = steady_clock::now ();
		if (paced)
		{
			target.set_baseline (begin);
			strategy->start ();
		}
		control.start.store (true, std::memory_order_release);

		uint64_t offered = 0;
		for (size_t round = 0; round < load.rounds; ++round)
		{
			while (control.finished.load (std::memory_order_acquire) < producer_count * (round + 1))
			{
				std::this_thread::yield ();
			}
			for (auto const & plan : plans)
			{
				offered += plan.per_round;
			}

			auto const drain_start = steady_clock::now ();
			if (!paced)
			{
				target.set_baseline (drain_start);
				strategy->start ();
			}
			// Drained: everything handed over so far was either processed or refused
			while (target.processed () + dropped_total (stats) < offered)
			{
				std::this_thread::yield ();
			}
			if (!paced)
			{
				result.elapsed += steady_clock::now () - drain_start;
				strategy->stop ();
			}
			control.released.store (round + 1, std::memory_order_release);
		}
		if (paced)
		{
			result.elapsed = steady_clock::now () - begin;
		}

		for (auto & producer : producers)
		{
			producer.join ();
		}
		strategy->stop ();
	}

	result.offered = total;
	result.processed = target.processed ();
	result.dropped = dropped_total (stats);
	result.reorders = target.reorders ();
	for (auto const & producer : stats)
	{
		result.dropped_hot += producer.dropped_hot.load (std::memory_order_relaxed);
		result.busy += producer.busy;
	}

	auto const & latencies = target.latencies ();
	std::vector<uint32_t> all;
	std::vector<uint32_t> hot;
	std::vector<uint32_t> cold;
	all.reserve (latencies.size ());
	for (auto const & plan : plans)
	{
		for (size_t position = 0; position < plan.picks.size (); ++position)
		{
			bool const is_hot = plan.picks[position].channel == 0;
			result.offered_hot += is_hot;
			auto const latency = latencies[plan.first_index + position];
			if (latency == 0)
			{
				continue; // Dropped
			}
			all.push_back (latency);
			(is_hot ? hot : cold).push_back (latency);
		}
	}
	result.all = summarize (all);
	result.hot = summarize (hot);
	result.cold = summarize (cold);
	return result;
}

struct strategy_entry
{
	std::string name;
	strategy_factory make;
	bool threaded; // Takes a thread count
	bool queued; // Holds messages until started, so it can drain a burst
};

std::vector<strategy_entry> const & strategies ()
{
	static std::vector<strategy_entry> const list{
		{ "inline", [] (message_handler handler, size_t) { return std::make_unique<inline_strategy> (std::move (handler)); }, false, false },
		{ "fair_batch_4096", [] (message_handler handler, size_t threads) { return std::make_unique<fair_batch_strategy> (std::move (handler), threads, production_batch, lane_capacity); }, true, true },
		{ "fair_batch_8", [] (message_handler handler, size_t threads) { return std::make_unique<fair_batch_strategy> (std::move (handler), threads, 8, lane_capacity); }, true, true },
		{ "per_class", [] (message_handler handler, size_t) { return std::make_unique<per_class_strategy> (std::move (handler), production_batch, lane_capacity); }, false, true },
		{ "sharded_channel", [] (message_handler handler, size_t threads) { return std::make_unique<sharded_strategy> (std::move (handler), threads, order_key::channel, production_batch, lane_capacity); }, true, true },
		{ "sharded_channel_class", [] (message_handler handler, size_t threads) { return std::make_unique<sharded_strategy> (std::move (handler), threads, order_key::channel_class, production_batch, lane_capacity); }, true, true },
		{ "keyed_channel", [] (message_handler handler, size_t threads) { return std::make_unique<keyed_strategy> (std::move (handler), threads, order_key::channel, lane_batch, lane_capacity); }, true, true },
		{ "keyed_channel_class", [] (message_handler handler, size_t threads) { return std::make_unique<keyed_strategy> (std::move (handler), threads, order_key::channel_class, lane_batch, lane_capacity); }, true, true },
	};
	return list;
}

double percent (uint64_t part, uint64_t whole)
{
	return whole == 0 ? 0.0 : 100.0 * part / whole;
}

void run_benchmark (benchmark::State & state, strategy_entry const & strategy, workload const & load, work_profile const & profile)
{
	size_t const threads = strategy.threaded ? state.range (0) : 0;
	uint64_t processed = 0;
	for (auto _ : state)
	{
		auto const result = run_scenario (strategy.make, threads, load, profile);
		state.SetIterationTime (std::chrono::duration<double> (result.elapsed).count ());
		processed += result.processed;

		state.counters["p50_us"] = result.all.p50_us;
		state.counters["p99_us"] = result.all.p99_us;
		state.counters["max_us"] = result.all.max_us;
		state.counters["drop_pct"] = percent (result.dropped, result.offered);
		state.counters["reorder_pct"] = percent (result.reorders, result.processed);
		if (load.rate > 0)
		{
			state.counters["offered_per_s"] = result.offered / std::chrono::duration<double> (result.elapsed).count ();
		}
		else
		{
			state.counters["put_ns"] = static_cast<double> (result.busy.count ()) / result.offered;
		}
		if (load.hot_percent > 0)
		{
			state.counters["hot_p99_us"] = result.hot.p99_us;
			state.counters["cold_p99_us"] = result.cold.p99_us;
			state.counters["hot_drop_pct"] = percent (result.dropped_hot, result.offered_hot);
			state.counters["cold_drop_pct"] = percent (result.dropped - result.dropped_hot, result.offered - result.offered_hot);
		}
	}
	state.SetItemsProcessed (processed);
}

void register_family (std::string const & family, workload const & load, std::string const & profile_name, work_profile const & profile)
{
	for (auto const & strategy : strategies ())
	{
		if (load.rate == 0 && !strategy.queued)
		{
			continue; // Nothing to drain
		}
		auto const name = "BM_message_processing/" + family + "/" + profile_name + "/" + strategy.name;
		auto * registered = benchmark::RegisterBenchmark (name, [&strategy, load, &profile] (benchmark::State & state) {
			run_benchmark (state, strategy, load, profile);
		});
		registered->UseManualTime ()->Iterations (1)->Unit (benchmark::kMillisecond);
		if (strategy.threaded)
		{
			registered->ArgName ("threads");
			for (auto threads : thread_counts)
			{
				registered->Arg (threads);
			}
		}
	}
}

std::string rate_name (uint64_t rate)
{
	return rate >= 1'000'000 ? std::to_string (rate / 1'000'000) + "m" : std::to_string (rate / 1'000) + "k";
}

void register_all ()
{
	std::array<std::pair<std::string, work_profile const *>, 2> const profiles{ { { "light", &light_profile }, { "mixed", &mixed_profile } } };
	for (auto const & [profile_name, profile] : profiles)
	{
		workload const burst{ .rounds = burst_rounds, .messages = channel_count * lane_capacity };
		register_family ("burst", burst, profile_name, *profile);

		for (auto rate : paced_rates)
		{
			workload const paced{ .messages = static_cast<size_t> (rate * paced_duration / 1s), .rate = rate };
			register_family ("paced_" + rate_name (rate), paced, profile_name, *profile);
		}

		workload const hot{ .messages = static_cast<size_t> (hot_rate * paced_duration / 1s), .rate = hot_rate, .hot_percent = hot_percent };
		register_family ("hot", hot, profile_name, *profile);
	}
}

struct registrar
{
	registrar ()
	{
		register_all ();
	}
};
[[maybe_unused]] registrar const registrar_instance;
}
