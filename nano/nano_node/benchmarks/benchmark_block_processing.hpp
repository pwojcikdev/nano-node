#pragma once

#include <nano/lib/blocks.hpp>
#include <nano/lib/locks.hpp>
#include <nano/nano_node/benchmarks/benchmark_common.hpp>

#include <boost/program_options.hpp>

#include <atomic>
#include <memory>
#include <unordered_set>

namespace nano::cli
{
class block_processing_benchmark : public benchmark_base
{
private:
	// Blocks currently being processed
	nano::locked<std::unordered_set<nano::block_hash>> current_blocks;

	// Metrics
	std::atomic<size_t> processed_blocks_count{ 0 };

public:
	block_processing_benchmark (std::shared_ptr<nano::node> node_a, benchmark_config const & config_a);

	void run_benchmark ();
	void measure_processing_performance (std::deque<std::shared_ptr<nano::block>> & blocks);
	void print_statistics ();

	static void run (boost::program_options::variables_map const & vm, std::filesystem::path const & data_path);
};
}