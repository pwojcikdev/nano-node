#include <nano/benchmarks/allocations.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <string>

#if defined(__APPLE__)
#include <pthread.h>

// libmalloc calls this hook, when set, on every allocation and release of every thread; MallocStackLogging is built on it
using malloc_logger_t = void (uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t result, uint32_t num_hot_frames_to_skip);
extern "C" malloc_logger_t * malloc_logger;
#endif

// A sanitizer runtime defines operator new itself and the linker rejects a second definition
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define NANO_BENCHMARKS_SANITIZED 1
#endif
#elif defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define NANO_BENCHMARKS_SANITIZED 1
#endif

namespace
{
// Constant initialized, so reading them never allocates
thread_local uint64_t allocation_count{ 0 };
thread_local uint64_t allocation_bytes{ 0 };

// Threads share a fixed set of counters picked by a hash of the thread, a counter of its own would have to be allocated inside the hook
struct alignas (128) process_counter
{
	std::atomic<uint64_t> count{ 0 };
	std::atomic<uint64_t> bytes{ 0 };
};

std::array<process_counter, 64> process_counters;

#if defined(__APPLE__)
malloc_logger_t * replaced_logger{ nullptr };

void count_malloc (uint32_t type, uintptr_t, uintptr_t arg2, uintptr_t arg3, uintptr_t, uint32_t)
{
	uint32_t const log_allocate = 2;
	uint32_t const log_deallocate = 4; // Set together with log_allocate by realloc, which passes the old pointer before the new size
	if ((type & log_allocate) == 0)
	{
		return;
	}

	auto const size = (type & log_deallocate) != 0 ? arg3 : arg2;
	auto const thread = reinterpret_cast<uintptr_t> (pthread_self ());
	auto & counter = process_counters[(thread * 0x9e3779b97f4a7c15ull) >> 58];
	counter.count.fetch_add (1, std::memory_order_relaxed);
	counter.bytes.fetch_add (size, std::memory_order_relaxed);
}
#endif
}

nano::benchmarks::allocation_stats nano::benchmarks::thread_allocations ()
{
	return { allocation_count, allocation_bytes };
}

nano::benchmarks::allocation_stats nano::benchmarks::process_allocations ()
{
	allocation_stats result;
	for (auto const & counter : process_counters)
	{
		result.count += counter.count.load (std::memory_order_relaxed);
		result.bytes += counter.bytes.load (std::memory_order_relaxed);
	}
	return result;
}

nano::benchmarks::process_allocation_tracker::process_allocation_tracker ()
{
#if defined(__APPLE__)
	replaced_logger = malloc_logger;
	malloc_logger = count_malloc;
#endif
}

nano::benchmarks::process_allocation_tracker::~process_allocation_tracker ()
{
#if defined(__APPLE__)
	malloc_logger = replaced_logger;
#endif
}

nano::benchmarks::allocation_counter::allocation_counter (std::string_view unit_a) :
	unit{ unit_a }
{
}

void nano::benchmarks::allocation_counter::report (benchmark::State & state) const
{
	auto const used = thread_allocations () - start;
	auto const iterations = static_cast<double> (state.iterations ());
	std::string const unit_l{ unit };
	state.counters["allocs/" + unit_l] = benchmark::Counter (used.count / iterations, benchmark::Counter::kAvgThreads);
	state.counters["bytes/" + unit_l] = benchmark::Counter (used.bytes / iterations, benchmark::Counter::kAvgThreads);
	// Rates are divided by the summed up time of all threads, scaling by the thread count turns that into the total throughput
	state.counters[unit_l + "s/s"] = benchmark::Counter (iterations * state.threads (), benchmark::Counter::kIsRate);
}

#ifndef NANO_BENCHMARKS_SANITIZED

/*
 * Replacing the plain forms is enough: the default array and nothrow forms forward to them.
 * The aligned forms keep their defaults and stay uncounted, which keeps allocation and release paired on every platform.
 */
void * operator new (std::size_t size)
{
	++allocation_count;
	allocation_bytes += size;
	if (auto * result = std::malloc (size != 0 ? size : 1))
	{
		return result;
	}
	throw std::bad_alloc{};
}

void operator delete (void * pointer) noexcept
{
	std::free (pointer);
}

void operator delete (void * pointer, std::size_t) noexcept
{
	std::free (pointer);
}

#endif
