#pragma once

#include <nano/lib/blocks.hpp>
#include <nano/lib/locks.hpp>
#include <nano/nano_node/benchmarks/benchmark_common.hpp>

#include <boost/program_options.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>

namespace nano::cli
{
class pipeline_benchmark : public benchmark_base
{
private:
	struct block_timing
	{
		std::chrono::steady_clock::time_point submitted;
		std::chrono::steady_clock::time_point processed;
		std::chrono::steady_clock::time_point election_started;
		std::chrono::steady_clock::time_point election_stopped;
		std::chrono::steady_clock::time_point confirmed;
		std::chrono::steady_clock::time_point cemented;
	};

	// Track timing for each block through the pipeline
	nano::locked<std::unordered_map<nano::block_hash, block_timing>> block_timings;

	// Track blocks waiting to be cemented
	nano::locked<std::unordered_map<nano::block_hash, std::chrono::steady_clock::time_point>> pending_cementing;

	// Metrics
	std::atomic<size_t> processed_blocks_count{ 0 };
	std::atomic<size_t> elections_started{ 0 };
	std::atomic<size_t> elections_stopped{ 0 };
	std::atomic<size_t> elections_confirmed{ 0 };
	std::atomic<size_t> blocks_cemented{ 0 };

	// Timing accumulators
	std::atomic<uint64_t> total_processing_time_us{ 0 };
	std::atomic<uint64_t> total_election_time_us{ 0 };
	std::atomic<uint64_t> total_election_stopped_time_us{ 0 };
	std::atomic<uint64_t> total_cementing_time_us{ 0 };
	std::atomic<uint64_t> total_confirmation_time_us{ 0 };

public:
	pipeline_benchmark (std::shared_ptr<nano::node> node_a, benchmark_config const & config_a);

	void run_benchmark ();
	void measure_confirmation_performance (std::deque<std::shared_ptr<nano::block>> & blocks);
	void print_statistics ();
	void print_timing_statistics ();

	static void run (boost::program_options::variables_map const & vm, std::filesystem::path const & data_path);
};
}