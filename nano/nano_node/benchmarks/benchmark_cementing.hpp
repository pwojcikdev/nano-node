#pragma once

#include <nano/lib/blocks.hpp>
#include <nano/lib/locks.hpp>
#include <nano/nano_node/benchmarks/benchmark_common.hpp>

#include <boost/program_options.hpp>

#include <atomic>
#include <memory>
#include <unordered_map>

namespace nano::cli
{
class cementing_benchmark : public benchmark_base
{
private:
	// Track blocks waiting to be cemented
	nano::locked<std::unordered_map<nano::block_hash, std::chrono::steady_clock::time_point>> pending_cementing;

	// Metrics
	std::atomic<size_t> processed_blocks_count{ 0 };
	std::atomic<size_t> cemented_blocks_count{ 0 };

public:
	cementing_benchmark (std::shared_ptr<nano::node> node_a, benchmark_config const & config_a);

	void run_benchmark ();
	void measure_cementing_performance (std::deque<std::shared_ptr<nano::block>> & blocks);
	void print_statistics ();

	static void run (boost::program_options::variables_map const & vm, std::filesystem::path const & data_path);
};
}