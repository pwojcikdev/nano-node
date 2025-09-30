#include <nano/lib/config.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/thread_runner.hpp>
#include <nano/lib/timer.hpp>
#include <nano/nano_node/benchmarks/benchmarks.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/cli.hpp>
#include <nano/node/daemonconfig.hpp>
#include <nano/node/election.hpp>
#include <nano/node/ledger_notifications.hpp>
#include <nano/node/node_observers.hpp>
#include <nano/node/transport/tcp_server.hpp>
#include <nano/secure/ledger.hpp>

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <iostream>
#include <limits>
#include <numeric>
#include <thread>

#include <fmt/format.h>

namespace nano::cli
{
struct node_metrics
{
	std::atomic<size_t> blocks_processed{ 0 };
	std::atomic<size_t> elections_started{ 0 };
	std::atomic<size_t> elections_stopped{ 0 };
	std::atomic<size_t> elections_confirmed{ 0 };
	std::atomic<size_t> blocks_cemented{ 0 };
};

class multi_node_pipeline_benchmark : public benchmark_base
{
private:
	struct block_timing
	{
		std::chrono::steady_clock::time_point submitted;
		std::chrono::steady_clock::time_point processed;
		std::chrono::steady_clock::time_point election_started;
		std::chrono::steady_clock::time_point cemented;
	};

	// Shared IO context for all nodes
	std::shared_ptr<boost::asio::io_context> io_ctx;
	nano::work_pool work_pool;

	// Multiple nodes
	std::vector<std::shared_ptr<nano::node>> nodes;
	std::vector<std::unique_ptr<nano::thread_runner>> runners;
	std::vector<std::unique_ptr<node_metrics>> metrics;

	// Track timing for each block
	nano::locked<std::unordered_map<nano::block_hash, block_timing>> block_timings;

	// Track blocks waiting to be cemented on all nodes
	nano::locked<std::unordered_map<nano::block_hash, std::chrono::steady_clock::time_point>> pending_cementing;

	size_t num_representatives;
	size_t num_non_representatives;

public:
	multi_node_pipeline_benchmark (benchmark_config const & config_a);
	~multi_node_pipeline_benchmark ();

	void setup_nodes (nano::node_flags const & node_flags);
	void setup_network_connections ();
	void setup_voting_weight ();
	void run ();
	void run_iteration (std::deque<std::shared_ptr<nano::block>> & blocks);
	void print_statistics ();
};

void run_multi_node_pipeline_benchmark (boost::program_options::variables_map const & vm, std::filesystem::path const & data_path)
{
	auto config = benchmark_config::parse (vm);

	std::cout << "=== BENCHMARK: Multi-Node Pipeline ===\n";
	std::cout << "Configuration:\n";
	std::cout << fmt::format ("  Representatives: {}\n", config.num_representatives);
	std::cout << fmt::format ("  Non-representatives: {}\n", config.num_non_representatives);
	std::cout << fmt::format ("  Total nodes: {}\n", config.num_representatives + config.num_non_representatives);
	std::cout << fmt::format ("  Accounts: {}\n", config.num_accounts);
	std::cout << fmt::format ("  Iterations: {}\n", config.num_iterations);
	std::cout << fmt::format ("  Batch size: {}\n", config.batch_size);

	// Setup network and logger
	nano::network_constants::set_active_network ("dev");
	nano::logger::initialize (nano::log_config::cli_default (nano::log::level::warn));

	nano::node_flags node_flags;
	nano::update_flags (node_flags, vm);

	// Run benchmark
	multi_node_pipeline_benchmark benchmark{ config };
	benchmark.setup_nodes (node_flags);
	benchmark.setup_network_connections ();
	benchmark.setup_voting_weight ();
	benchmark.run ();
}

multi_node_pipeline_benchmark::multi_node_pipeline_benchmark (benchmark_config const & config_a) :
	benchmark_base (nullptr, config_a),
	io_ctx (std::make_shared<boost::asio::io_context> ()),
	work_pool (nano::dev::network_params.network, std::numeric_limits<unsigned>::max ()),
	num_representatives (config_a.num_representatives),
	num_non_representatives (config_a.num_non_representatives)
{
}

multi_node_pipeline_benchmark::~multi_node_pipeline_benchmark ()
{
	// Stop all nodes
	for (auto & node : nodes)
	{
		node->stop ();
	}
}

void multi_node_pipeline_benchmark::setup_nodes (nano::node_flags const & node_flags)
{
	size_t total_nodes = num_representatives + num_non_representatives;
	std::cout << fmt::format ("\nCreating {} nodes ({} representatives, {} non-representatives)...\n",
	total_nodes, num_representatives, num_non_representatives);

	// Load base configuration
	auto daemon_config = nano::load_config_file<nano::daemon_config> (nano::node_config_filename, {}, node_flags.config_overrides);

	for (size_t i = 0; i < total_nodes; ++i)
	{
		auto node_config = daemon_config.node;
		node_config.network_params.work = nano::work_thresholds{ 0, 0, 0 };
		node_config.peering_port = 0; // Use random available port
		node_config.max_backlog = 0; // Disable bounded backlog
		node_config.block_processor.max_peer_queue = std::numeric_limits<size_t>::max ();
		node_config.block_processor.max_system_queue = std::numeric_limits<size_t>::max ();
		node_config.max_unchecked_blocks = 1024 * 1024;
		node_config.vote_processor.max_pr_queue = std::numeric_limits<size_t>::max ();

		// Create node with unique data path
		auto node = std::make_shared<nano::node> (io_ctx, nano::unique_path (), node_config, work_pool, node_flags);

		// Create metrics for this node
		auto node_metrics_ptr = std::make_unique<node_metrics> ();
		auto & m = *node_metrics_ptr;

		// Setup observers for this node
		node->ledger_notifications.blocks_processed.add ([&m, this] (std::deque<std::pair<nano::block_status, nano::block_context>> const & batch) {
			auto now = std::chrono::steady_clock::now ();
			auto timings_l = block_timings.lock ();

			for (auto const & [status, context] : batch)
			{
				if (status == nano::block_status::progress)
				{
					if (auto it = timings_l->find (context.block->hash ()); it != timings_l->end ())
					{
						if (it->second.processed == std::chrono::steady_clock::time_point{})
						{
							it->second.processed = now;
						}
					}
					m.blocks_processed++;
				}
			}
		});

		node->active.election_started.add ([&m, this] (std::shared_ptr<nano::election> const & election, nano::bucket_index const &, nano::priority_timestamp const &) {
			auto now = std::chrono::steady_clock::now ();
			auto hash = election->winner ()->hash ();
			auto timings_l = block_timings.lock ();

			if (auto it = timings_l->find (hash); it != timings_l->end ())
			{
				if (it->second.election_started == std::chrono::steady_clock::time_point{})
				{
					it->second.election_started = now;
				}
			}

			m.elections_started++;
		});

		node->active.election_erased.add ([&m] (std::shared_ptr<nano::election> const & election) {
			m.elections_stopped++;
			m.elections_confirmed += election->confirmed () ? 1 : 0;
		});

		node->cementing_set.batch_cemented.add ([&m, this] (auto const & hashes) {
			auto now = std::chrono::steady_clock::now ();
			auto pending_l = pending_cementing.lock ();
			auto timings_l = block_timings.lock ();

			for (auto const & ctx : hashes)
			{
				auto hash = ctx.block->hash ();

				if (auto it = timings_l->find (hash); it != timings_l->end ())
				{
					if (it->second.cemented == std::chrono::steady_clock::time_point{})
					{
						it->second.cemented = now;
					}
				}
				pending_l->erase (hash);

				m.blocks_cemented++;
			}
		});

		node->start ();

		nodes.push_back (node);
		metrics.push_back (std::move (node_metrics_ptr));
	}

	// Create thread runners for IO context
	size_t io_threads = nodes[0]->config.io_threads;
	for (size_t i = 0; i < io_threads; ++i)
	{
		runners.push_back (std::make_unique<nano::thread_runner> (io_ctx, nano::default_logger ()));
	}

	std::cout << "All nodes created and started\n";
}

void multi_node_pipeline_benchmark::setup_network_connections ()
{
	std::cout << "Establishing network connections between nodes...\n";

	// Connect each node to every other node
	for (size_t i = 0; i < nodes.size (); ++i)
	{
		for (size_t j = i + 1; j < nodes.size (); ++j)
		{
			auto & node1 = *nodes[i];
			auto & node2 = *nodes[j];

			// Establish TCP connection from node1 to node2
			node1.network.merge_peer (node2.network.endpoint ());
		}
	}

	// Wait for connections to establish
	std::this_thread::sleep_for (std::chrono::seconds (2));

	// Print connection status
	size_t total_peers = 0;
	for (size_t i = 0; i < nodes.size (); ++i)
	{
		auto peer_count = nodes[i]->network.size ();
		total_peers += peer_count;
		std::cout << fmt::format ("  Node {}: {} peers\n", i, peer_count);
	}

	std::cout << fmt::format ("Network established ({} total peer connections)\n", total_peers / 2);
}

void multi_node_pipeline_benchmark::setup_voting_weight ()
{
	std::cout << "Distributing voting weight to representatives...\n";

	// Only set up the first node (which has genesis) initially
	node = nodes[0];

	// Generate keypairs for representatives
	std::vector<nano::keypair> rep_keys;
	for (size_t i = 0; i < num_representatives; ++i)
	{
		rep_keys.emplace_back ();
	}

	// Get genesis balance
	nano::block_hash genesis_latest (nodes[0]->latest (nano::dev::genesis_key.pub));
	nano::uint128_t genesis_balance (std::numeric_limits<nano::uint128_t>::max ());

	// Calculate amount to distribute to each representative
	// Keep some for genesis, distribute rest equally among representatives
	nano::uint128_t total_to_distribute = genesis_balance * 90 / 100; // 90% to representatives
	nano::uint128_t per_rep = total_to_distribute / num_representatives;

	nano::block_builder builder;
	nano::uint128_t remaining_genesis = genesis_balance;

	// Send and open blocks for each representative
	for (size_t i = 0; i < num_representatives; ++i)
	{
		// Send from genesis to representative
		remaining_genesis -= per_rep;
		auto send = builder.state ()
					.account (nano::dev::genesis_key.pub)
					.previous (genesis_latest)
					.representative (nano::dev::genesis_key.pub)
					.balance (remaining_genesis)
					.link (rep_keys[i].pub)
					.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
					.work (0)
					.build ();

		auto result1 = nodes[0]->process (send);
		release_assert (result1 == nano::block_status::progress, to_string (result1));
		genesis_latest = send->hash ();

		// Open block for representative
		auto open = builder.state ()
					.account (rep_keys[i].pub)
					.previous (0)
					.representative (rep_keys[i].pub)
					.balance (per_rep)
					.link (send->hash ())
					.sign (rep_keys[i].prv, rep_keys[i].pub)
					.work (0)
					.build ();

		auto result2 = nodes[0]->process (open);
		release_assert (result2 == nano::block_status::progress, to_string (result2));

		// Confirm these blocks on the first node
		{
			auto transaction = nodes[0]->ledger.tx_begin_write ();
			nodes[0]->ledger.confirm (transaction, send->hash ());
			nodes[0]->ledger.confirm (transaction, open->hash ());
		}
	}

	// Insert representative keys into wallets of representative nodes
	for (size_t i = 0; i < num_representatives; ++i)
	{
		auto wallet = nodes[i]->wallets.create (nano::random_wallet_id ());
		wallet->insert_adhoc (rep_keys[i].prv);
	}

	// Also insert genesis key into first node's wallet for voting
	auto genesis_wallet = nodes[0]->wallets.create (nano::random_wallet_id ());
	genesis_wallet->insert_adhoc (nano::dev::genesis_key.prv);

	// Wait for blocks to propagate
	std::this_thread::sleep_for (std::chrono::seconds (1));

	// std::cout << fmt::format ("Voting weight distributed: {} per representative ({:.1f}% each)\n",
	// per_rep, 100.0 * per_rep / genesis_balance);

	// Transfer remaining genesis balance to pool account for benchmark
	pool.generate_accounts (config.num_accounts);

	// Get random account from pool
	nano::account pool_account = pool.get_random_account ();
	auto & pool_keypair = pool.get_keypair (pool_account);

	// Send remaining balance to pool account
	auto send = builder.state ()
				.account (nano::dev::genesis_key.pub)
				.previous (genesis_latest)
				.representative (nano::dev::genesis_key.pub)
				.balance (0)
				.link (pool_account)
				.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
				.work (0)
				.build ();

	auto result1 = nodes[0]->process (send);
	release_assert (result1 == nano::block_status::progress, to_string (result1));

	// Open block for pool account
	auto open = builder.state ()
				.account (pool_account)
				.previous (0)
				.representative (pool_account)
				.balance (remaining_genesis)
				.link (send->hash ())
				.sign (pool_keypair.prv, pool_keypair.pub)
				.work (0)
				.build ();

	auto result2 = nodes[0]->process (open);
	release_assert (result2 == nano::block_status::progress, to_string (result2));

	// Update pool tracking
	pool.set_initial_balance (pool_account, remaining_genesis);
	pool.set_frontier (pool_account, open->hash ());

	// Wait for setup to complete
	std::this_thread::sleep_for (std::chrono::seconds (1));

	std::cout << fmt::format ("Setup complete. Pool account has {} balance for testing\n", remaining_genesis);
}

void multi_node_pipeline_benchmark::run ()
{
	for (size_t iteration = 0; iteration < config.num_iterations; ++iteration)
	{
		std::cout << fmt::format ("\n--- Iteration {}/{} --------------------------------------------------------------\n", iteration + 1, config.num_iterations);
		std::cout << fmt::format ("Generating {} random transfers...\n", config.batch_size / 2);
		auto blocks = generate_random_transfers ();

		std::cout << fmt::format ("Measuring multi-node pipeline for {} blocks...\n", blocks.size ());
		run_iteration (blocks);
	}

	print_statistics ();
}

void multi_node_pipeline_benchmark::run_iteration (std::deque<std::shared_ptr<nano::block>> & blocks)
{
	auto const total_blocks = blocks.size ();

	// Initialize timing entries for all blocks
	{
		auto now = std::chrono::steady_clock::now ();
		auto timings_l = block_timings.lock ();
		auto pending_l = pending_cementing.lock ();
		for (auto const & block : blocks)
		{
			timings_l->emplace (block->hash (), block_timing{ now });
			// Track cementing on all nodes (will be removed as each node cements)
			for (size_t i = 0; i < nodes.size (); ++i)
			{
				pending_l->emplace (block->hash (), now);
			}
		}
	}

	auto const time_begin = std::chrono::high_resolution_clock::now ();

	// Submit all blocks to the first node through the full pipeline
	std::cout << "Submitting blocks to first node...\n";
	while (!blocks.empty ())
	{
		auto block = blocks.front ();
		blocks.pop_front ();

		// Process block through full confirmation pipeline
		nodes[0]->process_active (block);
	}

	// Wait for all blocks to be confirmed and cemented on all nodes
	nano::interval progress_interval;
	size_t last_remaining = total_blocks * nodes.size ();
	while (true)
	{
		{
			auto pending_l = pending_cementing.lock ();
			size_t remaining = pending_l->size ();

			if (remaining == 0 || progress_interval.elapse (3s))
			{
				// Count per-node status
				std::cout << fmt::format ("Blocks remaining to cement: {:>9} across all nodes\n", remaining);
				for (size_t i = 0; i < nodes.size (); ++i)
				{
					std::cout << fmt::format ("  Node {}: block_processor={:>6} | active={:>5} | cementing={:>5}\n",
					i,
					nodes[i]->block_processor.size (),
					nodes[i]->active.size (),
					nodes[i]->cementing_set.size ());
				}
			}

			if (remaining == 0)
			{
				break;
			}

			last_remaining = remaining;
		}

		std::this_thread::sleep_for (std::chrono::milliseconds (10));
	}

	auto const time_end = std::chrono::high_resolution_clock::now ();
	auto const time_us = std::chrono::duration_cast<std::chrono::microseconds> (time_end - time_begin).count ();

	std::cout << fmt::format ("\nPerformance: {} blocks/sec [{:.2f}s] {} blocks processed\n",
	total_blocks * 1000000 / time_us, time_us / 1000000.0, total_blocks);
	std::cout << "─────────────────────────────────────────────────────────────────\n";

	// Clear stats for all nodes
	for (auto & node : nodes)
	{
		node->stats.clear ();
	}
}

void multi_node_pipeline_benchmark::print_statistics ()
{
	std::cout << "\n--- SUMMARY ---------------------------------------------------------------------\n\n";

	// Per-node statistics
	std::cout << "Per-Node Statistics:\n";
	for (size_t i = 0; i < nodes.size (); ++i)
	{
		auto & m = *metrics[i];
		std::cout << fmt::format ("\nNode {} {}:\n",
		i, i < num_representatives ? "(representative)" : "(non-representative)");
		std::cout << fmt::format ("  Blocks processed:    {:>10}\n", m.blocks_processed.load ());
		std::cout << fmt::format ("  Elections started:   {:>10}\n", m.elections_started.load ());
		std::cout << fmt::format ("  Elections confirmed: {:>10}\n", m.elections_confirmed.load ());
		std::cout << fmt::format ("  Blocks cemented:     {:>10}\n", m.blocks_cemented.load ());
	}

	// Aggregate statistics
	std::cout << "\nAggregate Statistics:\n";
	size_t total_processed = 0;
	size_t total_elections = 0;
	size_t total_confirmed = 0;
	size_t total_cemented = 0;

	for (auto & m : metrics)
	{
		total_processed += m->blocks_processed.load ();
		total_elections += m->elections_started.load ();
		total_confirmed += m->elections_confirmed.load ();
		total_cemented += m->blocks_cemented.load ();
	}

	std::cout << fmt::format ("  Total blocks processed:    {:>10}\n", total_processed);
	std::cout << fmt::format ("  Total elections started:   {:>10}\n", total_elections);
	std::cout << fmt::format ("  Total elections confirmed: {:>10}\n", total_confirmed);
	std::cout << fmt::format ("  Total blocks cemented:     {:>10}\n", total_cemented);

	// Timing statistics
	auto timings_l = block_timings.lock ();

	std::vector<uint64_t> processing_times;
	std::vector<uint64_t> activation_times;
	std::vector<uint64_t> confirmation_times;
	std::vector<uint64_t> total_times;

	for (auto const & [hash, timing] : *timings_l)
	{
		if (timing.processed != std::chrono::steady_clock::time_point{})
		{
			processing_times.push_back (std::chrono::duration_cast<std::chrono::microseconds> (timing.processed - timing.submitted).count ());
		}
		if (timing.election_started != std::chrono::steady_clock::time_point{} && timing.processed != std::chrono::steady_clock::time_point{})
		{
			activation_times.push_back (std::chrono::duration_cast<std::chrono::microseconds> (timing.election_started - timing.processed).count ());
		}
		if (timing.cemented != std::chrono::steady_clock::time_point{} && timing.election_started != std::chrono::steady_clock::time_point{})
		{
			confirmation_times.push_back (std::chrono::duration_cast<std::chrono::microseconds> (timing.cemented - timing.election_started).count ());
		}
		if (timing.cemented != std::chrono::steady_clock::time_point{})
		{
			total_times.push_back (std::chrono::duration_cast<std::chrono::microseconds> (timing.cemented - timing.submitted).count ());
		}
	}

	auto calc_stats = [] (std::vector<uint64_t> & times) {
		if (times.empty ())
			return std::make_tuple (0.0, 0.0, 0.0, 0.0);
		std::sort (times.begin (), times.end ());
		double avg = std::accumulate (times.begin (), times.end (), 0.0) / times.size ();
		double min = times.front ();
		double max = times.back ();
		double median = times[times.size () / 2];
		return std::make_tuple (avg, min, max, median);
	};

	std::cout << "\nTiming Statistics (milliseconds):\n";

	if (!processing_times.empty ())
	{
		auto [avg, min, max, median] = calc_stats (processing_times);
		std::cout << fmt::format ("  Block processing:  avg={:>8.2f}  min={:>8.2f}  max={:>8.2f}  median={:>8.2f}\n",
		avg / 1000.0, min / 1000.0, max / 1000.0, median / 1000.0);
	}

	if (!activation_times.empty ())
	{
		auto [avg, min, max, median] = calc_stats (activation_times);
		std::cout << fmt::format ("  Election activation:  avg={:>8.2f}  min={:>8.2f}  max={:>8.2f}  median={:>8.2f}\n",
		avg / 1000.0, min / 1000.0, max / 1000.0, median / 1000.0);
	}

	if (!confirmation_times.empty ())
	{
		auto [avg, min, max, median] = calc_stats (confirmation_times);
		std::cout << fmt::format ("  Confirmation time: avg={:>8.2f}  min={:>8.2f}  max={:>8.2f}  median={:>8.2f}\n",
		avg / 1000.0, min / 1000.0, max / 1000.0, median / 1000.0);
	}

	if (!total_times.empty ())
	{
		auto [avg, min, max, median] = calc_stats (total_times);
		std::cout << fmt::format ("  Total pipeline:    avg={:>8.2f}  min={:>8.2f}  max={:>8.2f}  median={:>8.2f}\n",
		avg / 1000.0, min / 1000.0, max / 1000.0, median / 1000.0);
	}
}
}