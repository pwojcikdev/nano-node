#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

namespace nano::benchmarks
{
/**
 * Heap allocations made through operator new, counted per thread.
 * Allocations made directly through malloc are not seen, and the counts stay at zero in sanitizer builds, where the sanitizer runtime owns operator new.
 */
struct allocation_stats
{
	uint64_t count{ 0 }; // Number of allocations
	uint64_t bytes{ 0 }; // Sum of the requested sizes

	allocation_stats operator- (allocation_stats const & other) const
	{
		return { count - other.count, bytes - other.bytes };
	}
};

// Allocations the calling thread has made since it started
allocation_stats thread_allocations ();

/**
 * Measures the allocations of the calling thread from construction until report, per iteration and as a rate.
 * `unit` names what one iteration does, so a unit of "vote" reports allocs/vote, bytes/vote and votes/s.
 */
class allocation_counter final
{
public:
	explicit allocation_counter (std::string_view unit) :
		unit{ unit }
	{
	}

	void report (benchmark::State & state) const
	{
		auto const used = thread_allocations () - start;
		auto const iterations = static_cast<double> (state.iterations ());
		std::string const unit_l{ unit };
		state.counters["allocs/" + unit_l] = benchmark::Counter (used.count / iterations, benchmark::Counter::kAvgThreads);
		state.counters["bytes/" + unit_l] = benchmark::Counter (used.bytes / iterations, benchmark::Counter::kAvgThreads);
		// Rates are divided by the summed up time of all threads, scaling by the thread count turns that into the total throughput
		state.counters[unit_l + "s/s"] = benchmark::Counter (iterations * state.threads (), benchmark::Counter::kIsRate);
	}

private:
	std::string_view unit;
	allocation_stats const start{ thread_allocations () };
};
}
