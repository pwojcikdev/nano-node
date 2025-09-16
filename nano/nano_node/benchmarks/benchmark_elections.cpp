#include <nano/lib/config.hpp>
#include <nano/lib/thread_runner.hpp>
#include <nano/lib/timer.hpp>
#include <nano/nano_node/benchmarks/benchmark_elections.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/cli.hpp>
#include <nano/node/daemonconfig.hpp>
#include <nano/node/ledger_notifications.hpp>
#include <nano/node/node_observers.hpp>
#include <nano/node/scheduler/component.hpp>
#include <nano/node/scheduler/manual.hpp>
#include <nano/secure/ledger.hpp>

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <iostream>
#include <limits>
#include <thread>

#include <fmt/format.h>

namespace nano::cli
{
elections_benchmark::elections_benchmark (std::shared_ptr<nano::node> node_a, benchmark_config const & config_a) :
	benchmark_base (node_a, config_a)
{
	// Track when elections start
	node->observers.active_started.add ([this] (nano::block_hash const & hash) {
		auto now = std::chrono::steady_clock::now ();
		auto locked = block_timings.lock ();

		if (auto it = locked->find (hash); it != locked->end ())
		{
			it->second.election_started = now;
		}
		elections_started++;
	});

	// Track when elections stop (regardless of confirmation)
	node->observers.active_stopped.add ([this] (nano::block_hash const & hash) {
		auto now = std::chrono::steady_clock::now ();
		auto locked = block_timings.lock ();

		if (auto it = locked->find (hash); it != locked->end ())
		{
			it->second.election_stopped = now;
			release_assert (it->second.election_started != std::chrono::steady_clock::time_point{});

			// Calculate election duration
			auto election_us = std::chrono::duration_cast<std::chrono::microseconds> (now - it->second.election_started).count ();
			total_election_stopped_time_us += election_us;
		}
		elections_stopped++;
	});

	// Track when blocks get cemented
	node->cementing_set.batch_cemented.add ([this] (auto const & hashes) {
		auto now = std::chrono::steady_clock::now ();
		auto pending_locked = pending_cementing.lock ();
		auto timings_locked = block_timings.lock ();

		for (auto const & ctx : hashes)
		{
			auto hash = ctx.block->hash ();
			pending_locked->erase (hash);

			// Update timing information
			if (auto it = timings_locked->find (hash); it != timings_locked->end ())
			{
				it->second.cemented = now;

				// Calculate total confirmation pipeline time (submission to cementing)
				auto confirmation_us = std::chrono::duration_cast<std::chrono::microseconds> (now - it->second.submitted).count ();
				total_confirmation_time_us += confirmation_us;

				// Calculate election time if we have election start
				if (it->second.election_started != std::chrono::steady_clock::time_point{})
				{
					auto election_us = std::chrono::duration_cast<std::chrono::microseconds> (it->second.cemented - it->second.election_started).count ();
					total_election_time_us += election_us;
				}
			}
			blocks_cemented++;
			elections_confirmed++; // Count cemented blocks as confirmed
		}
	});
}

void elections_benchmark::run_benchmark ()
{
	std::cout << fmt::format ("Generating {} accounts...\n", config.num_accounts);
	pool.generate_accounts (config.num_accounts);

	std::cout << "Setting up genesis distribution...\n";
	setup_genesis_distribution (0.1); // Only distribute 10%, keep 90% for voting weight

	for (size_t iteration = 0; iteration < config.num_iterations; ++iteration)
	{
		std::cout << fmt::format ("\n----------------------------------------\n");
		std::cout << fmt::format ("Generating batch {} of independent blocks...\n", iteration + 1);
		auto [sends, opens] = generate_independent_blocks ();

		std::cout << fmt::format ("Measuring elections performance for {} opens...\n", opens.size ());
		measure_elections_performance (sends, opens);

		std::cout << fmt::format ("Iteration {} complete.\n", iteration + 1);
		std::cout << fmt::format ("  Elections started: {}\n", elections_started.load ());
		std::cout << fmt::format ("  Elections stopped: {}\n", elections_stopped.load ());
		std::cout << fmt::format ("  Elections confirmed: {}\n", elections_confirmed.load ());
		std::cout << "----------------------------------------\n";
	}

	print_statistics ();
	print_timing_statistics ();
}

void elections_benchmark::measure_elections_performance (std::deque<std::shared_ptr<nano::block>> & sends, std::deque<std::shared_ptr<nano::block>> & opens)
{
	auto const total_opens = opens.size ();

	// Process and cement all send blocks directly
	std::cout << fmt::format ("Processing and cementing {} send blocks...\n", sends.size ());
	{
		auto transaction = node->ledger.tx_begin_write ();
		for (auto const & send : sends)
		{
			auto result = node->ledger.process (transaction, send);
			release_assert (result == nano::block_status::progress, to_string (result));

			// Add to cementing set for direct cementing
			auto cemented = node->ledger.confirm (transaction, send->hash ());
			release_assert (!cemented.empty () && cemented.back ()->hash () == send->hash ());
		}
	}

	// Process open blocks into ledger without confirming
	std::cout << fmt::format ("Processing {} open blocks into ledger...\n", opens.size ());
	{
		auto transaction = node->ledger.tx_begin_write ();
		for (auto const & open : opens)
		{
			auto result = node->ledger.process (transaction, open);
			release_assert (result == nano::block_status::progress, to_string (result));
		}
	}

	// Initialize timing entries for open blocks only
	{
		auto locked = block_timings.lock ();
		auto pending_locked = pending_cementing.lock ();
		auto now = std::chrono::steady_clock::now ();
		for (auto const & open : opens)
		{
			block_timing timing;
			timing.submitted = now;
			locked->emplace (open->hash (), timing);
			pending_locked->emplace (open->hash (), now);
		}
	}

	auto const time_begin = std::chrono::high_resolution_clock::now ();

	// Manually start elections for open blocks only
	std::cout << fmt::format ("Starting elections manually for {} open blocks...\n", opens.size ());
	for (auto const & open : opens)
	{
		// Use manual scheduler to start election
		node->scheduler.manual.push (open);
	}

	// Wait for all elections to complete and blocks to be cemented
	nano::interval progress_interval;
	while (true)
	{
		size_t current_confirmed = elections_confirmed.load ();
		size_t current_cemented = blocks_cemented.load ();
		size_t pending_count = 0;
		size_t active_count = node->active.size ();

		{
			auto pending_locked = pending_cementing.lock ();
			pending_count = pending_locked->size ();
		}

		// Done when no blocks are pending cementing and no active elections
		if (pending_count == 0 && active_count == 0)
		{
			break;
		}

		if (progress_interval.elapse (3s))
		{
			std::cout << fmt::format ("{}/{} confirmed, {}/{} cemented, {} pending, {} active elections (cementing: {}, deferred: {})\n",
			current_confirmed, total_opens,
			current_cemented, total_opens,
			pending_count,
			active_count,
			node->cementing_set.size (),
			node->cementing_set.deferred_size ());
		}

		std::this_thread::sleep_for (10ms);
	}

	auto const time_end = std::chrono::high_resolution_clock::now ();
	auto const time_us = std::chrono::duration_cast<std::chrono::microseconds> (time_end - time_begin).count ();

	std::cout << fmt::format ("Elections and cementing completed in {:.2f}s ({} blocks/sec)\n",
	time_us / 1000000.0, total_opens * 1000000 / time_us);

	node->stats.clear ();
}

void elections_benchmark::print_statistics ()
{
	std::cout << "\n=== Benchmark Statistics ===\n";
	std::cout << fmt::format ("Total accounts: {}\n", pool.total_accounts ());
	std::cout << fmt::format ("Accounts with balance: {}\n", pool.accounts_with_balance_count ());
	std::cout << fmt::format ("Total elections started: {}\n", elections_started.load ());
	std::cout << fmt::format ("Total elections stopped: {}\n", elections_stopped.load ());
	std::cout << fmt::format ("Total elections confirmed: {}\n", elections_confirmed.load ());
	std::cout << fmt::format ("Account utilization: {:.1f}%\n",
	100.0 * pool.accounts_with_balance_count () / pool.total_accounts ());
}

void elections_benchmark::print_timing_statistics ()
{
	size_t confirmed_count = elections_confirmed.load ();
	size_t stopped_count = elections_stopped.load ();

	if (confirmed_count == 0 && stopped_count == 0)
	{
		std::cout << "No elections completed, cannot calculate timing statistics\n";
		return;
	}

	std::cout << "\n=== Elections Timing Statistics ===\n";

	if (confirmed_count > 0)
	{
		std::cout << fmt::format ("Average election time (confirmed): {:.2f} ms\n",
		total_election_time_us.load () / (confirmed_count * 1000.0));
		std::cout << fmt::format ("Average total confirmation time: {:.2f} ms\n",
		total_confirmation_time_us.load () / (confirmed_count * 1000.0));
	}

	if (stopped_count > 0)
	{
		std::cout << fmt::format ("Average election duration (all stopped): {:.2f} ms\n",
		total_election_stopped_time_us.load () / (stopped_count * 1000.0));
	}
}

void elections_benchmark::run (boost::program_options::variables_map const & vm, std::filesystem::path const & data_path)
{
	auto config = benchmark_config::parse (vm);

	std::cout << fmt::format ("Starting elections benchmark with {} accounts, {} iterations, {} batch size\n",
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

	// Disable election schedulers and backlog scanning
	node_config.hinted_scheduler.enable = false;
	node_config.optimistic_scheduler.enable = false;
	node_config.priority_scheduler.enable = false;
	node_config.backlog_scan.enable = false;

	node_config.block_processor.max_peer_queue = std::numeric_limits<size_t>::max (); // Unlimited queue size
	node_config.block_processor.max_system_queue = std::numeric_limits<size_t>::max (); // Unlimited queue size
	node_config.max_unchecked_blocks = 1024 * 1024; // Large unchecked blocks cache to avoid dropping blocks
	node_config.vote_processor.max_pr_queue = std::numeric_limits<size_t>::max (); // Unlimited vote processing queue

	auto node = std::make_shared<nano::node> (io_ctx, nano::unique_path (), node_config, work_pool, node_flags);
	node->start ();
	nano::thread_runner runner (io_ctx, nano::default_logger (), node->config.io_threads);

	std::cout << fmt::format ("Backend: {}\n", node->store.vendor_get ());

	// Insert dev genesis representative key for voting
	auto wallet = node->wallets.create (nano::random_wallet_id ());
	wallet->insert_adhoc (nano::dev::genesis_key.prv);

	// Wait for node to be ready
	std::this_thread::sleep_for (500ms);

	// Run benchmark
	elections_benchmark benchmark{ node, config };
	benchmark.run_benchmark ();

	node->stop ();
}
}