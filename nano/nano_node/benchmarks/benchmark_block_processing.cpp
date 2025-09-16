#include <nano/lib/config.hpp>
#include <nano/lib/thread_runner.hpp>
#include <nano/lib/timer.hpp>
#include <nano/lib/work.hpp>
#include <nano/lib/work_version.hpp>
#include <nano/nano_node/benchmarks/benchmark_block_processing.hpp>
#include <nano/node/cli.hpp>
#include <nano/node/daemonconfig.hpp>
#include <nano/node/ledger_notifications.hpp>

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <iostream>
#include <limits>
#include <thread>

#include <fmt/format.h>

namespace nano::cli
{
block_processing_benchmark::block_processing_benchmark (std::shared_ptr<nano::node> node_a, benchmark_config const & config_a) :
	benchmark_base (node_a, config_a)
{
	node->ledger_notifications.blocks_processed.add ([this] (std::deque<std::pair<nano::block_status, nano::block_context>> const & batch) {
		auto lock = current_blocks.lock ();
		for (auto const & [status, context] : batch)
		{
			if (status == nano::block_status::progress)
			{
				lock->erase (context.block->hash ());
				processed_blocks_count++;
			}
			else
			{
				switch (status)
				{
					case nano::block_status::old:
						// Ignore, doesn't matter
						break;
					case nano::block_status::gap_previous:
						// Ignore, should be handled by unchecked map
						break;
					case nano::block_status::gap_source:
						// Ignore, should be handled by unchecked map
						break;
					default:
						std::cout << fmt::format ("Block processing failed: {} for block {}\n", to_string (status), context.block->hash ().to_string ());
				}
			}
		}
	});
}

void block_processing_benchmark::run_benchmark ()
{
	std::cout << fmt::format ("Generating {} accounts...\n", config.num_accounts);
	pool.generate_accounts (config.num_accounts);

	std::cout << "Setting up genesis distribution...\n";
	setup_genesis_distribution ();

	for (size_t iteration = 0; iteration < config.num_iterations; ++iteration)
	{
		std::cout << fmt::format ("\n----------------------------------------\n");
		std::cout << fmt::format ("Generating batch {} of random transfers...\n", iteration + 1);
		auto blocks = generate_random_transfers ();

		std::cout << fmt::format ("Processing {} blocks...\n", blocks.size ());
		measure_processing_performance (blocks);

		std::cout << fmt::format ("Iteration {} complete. Processed a total of {} blocks so far\n", iteration + 1, processed_blocks_count.load ());
		std::cout << "----------------------------------------\n";
	}

	print_statistics ();
}

void block_processing_benchmark::measure_processing_performance (std::deque<std::shared_ptr<nano::block>> & blocks)
{
	auto const total_blocks = blocks.size ();

	// Add all blocks to tracking set
	{
		auto lock = current_blocks.lock ();
		for (auto const & block : blocks)
		{
			lock->insert (block->hash ());
		}
	}

	auto const time_begin = std::chrono::high_resolution_clock::now ();

	// Process all blocks
	while (!blocks.empty ())
	{
		auto block = blocks.front ();
		blocks.pop_front ();

		bool added = node->block_processor.add (block, nano::block_source::test);
		release_assert (added, "failed to add block to processor");
	}

	// Wait for processing to complete
	nano::interval progress_interval;
	while (true)
	{
		{
			auto current_blocks_l = current_blocks.lock ();
			if (current_blocks_l->empty ())
			{
				break;
			}
			if (progress_interval.elapse (3s))
			{
				std::cout << fmt::format ("{} blocks remaining to process (block processor: {}, unchecked: {})\n",
				current_blocks_l->size (),
				node->block_processor.size (),
				node->unchecked.count ());
			}
		}

		std::this_thread::sleep_for (10ms);
	}

	auto const time_end = std::chrono::high_resolution_clock::now ();
	auto const time_us = std::chrono::duration_cast<std::chrono::microseconds> (time_end - time_begin).count ();

	std::cout << fmt::format ("Processing completed in {:.2f}s ({} blocks/sec)\n",
	time_us / 1000000.0, total_blocks * 1000000 / time_us);

	node->stats.clear ();
}

void block_processing_benchmark::print_statistics ()
{
	std::cout << "\n=== Benchmark Statistics ===\n";
	std::cout << fmt::format ("Total accounts: {}\n", pool.total_accounts ());
	std::cout << fmt::format ("Accounts with balance: {}\n", pool.accounts_with_balance_count ());
	std::cout << fmt::format ("Total blocks processed: {}\n", processed_blocks_count.load ());
	std::cout << fmt::format ("Account utilization: {:.1f}%\n",
	100.0 * pool.accounts_with_balance_count () / pool.total_accounts ());
}

void block_processing_benchmark::run (boost::program_options::variables_map const & vm, std::filesystem::path const & data_path)
{
	auto config = benchmark_config::parse (vm);

	std::cout << fmt::format ("Starting block processing benchmark with {} accounts, {} iterations, {} batch size\n",
	config.num_accounts, config.num_iterations, config.batch_size);

	// Setup node directly in run method
	nano::network_constants::set_active_network ("dev");
	nano::logger::initialize (nano::log_config::cli_default (nano::log::level::warn));

	nano::node_flags node_flags;
	nano::update_flags (node_flags, vm);

	auto io_ctx = std::make_shared<boost::asio::io_context> ();
	nano::work_pool work_pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };

	// Load configuration from current working directory (if exists) and cli config overrides
	auto daemon_config = nano::load_config_file<nano::daemon_config> (nano::node_config_filename, {}, node_flags.config_overrides);
	auto node_config = daemon_config.node;
	node_config.peering_port = 0; // Use random available port
	node_config.max_backlog = 0; // Disable bounded backlog
	node_config.block_processor.max_system_queue = std::numeric_limits<size_t>::max (); // Unlimited queue size
	node_config.max_unchecked_blocks = 1024 * 1024; // Large unchecked blocks cache to avoid dropping blocks

	auto node = std::make_shared<nano::node> (io_ctx, nano::unique_path (), node_config, work_pool, node_flags);
	node->start ();
	nano::thread_runner runner (io_ctx, nano::default_logger (), node->config.io_threads);

	std::cout << fmt::format ("Backend: {}\n", node->store.vendor_get ());
	std::cout << fmt::format ("Using {} block processor threads, batch size: {}\n",
	1, // node->config.block_processor.threads,
	node->config.block_processor.batch_size);

	// Wait for node to be ready
	std::this_thread::sleep_for (500ms);

	// Run benchmark
	block_processing_benchmark benchmark{ node, config };
	benchmark.run_benchmark ();

	node->stop ();
}
}