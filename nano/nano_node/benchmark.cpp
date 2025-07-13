#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/thread_runner.hpp>
#include <nano/lib/timer.hpp>
#include <nano/lib/work.hpp>
#include <nano/lib/work_version.hpp>
#include <nano/nano_node/benchmark.hpp>
#include <nano/node/cli.hpp>
#include <nano/node/ledger_notifications.hpp>
#include <nano/node/node.hpp>
#include <nano/secure/ledger.hpp>

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <iostream>
#include <limits>
#include <thread>

#include <fmt/format.h>

namespace nano
{
throughput_account_pool::throughput_account_pool () :
	gen (rd ())
{
}

void throughput_account_pool::generate_accounts (size_t count)
{
	keys.clear ();
	keys.reserve (count);
	account_to_keypair.clear ();
	balances.clear ();
	accounts_with_balance.clear ();
	balance_lookup.clear ();

	for (size_t i = 0; i < count; ++i)
	{
		keys.emplace_back ();
		account_to_keypair[keys[i].pub] = keys[i];
		balances[keys[i].pub] = 0;
	}
}

nano::account throughput_account_pool::get_random_account_with_balance ()
{
	debug_assert (!accounts_with_balance.empty ());
	std::uniform_int_distribution<size_t> dist (0, accounts_with_balance.size () - 1);
	return accounts_with_balance[dist (gen)];
}

nano::account throughput_account_pool::get_random_account ()
{
	debug_assert (!keys.empty ());
	std::uniform_int_distribution<size_t> dist (0, keys.size () - 1);
	return keys[dist (gen)].pub;
}

nano::keypair const & throughput_account_pool::get_keypair (nano::account const & account)
{
	auto it = account_to_keypair.find (account);
	debug_assert (it != account_to_keypair.end ());
	return it->second;
}

void throughput_account_pool::update_balance (nano::account const & account, nano::uint128_t new_balance)
{
	auto old_balance = balances[account];
	balances[account] = new_balance;

	bool had_balance = balance_lookup.count (account) > 0;
	bool has_balance_now = new_balance > 0;

	if (!had_balance && has_balance_now)
	{
		// Account gained balance
		accounts_with_balance.push_back (account);
		balance_lookup.insert (account);
	}
	else if (had_balance && !has_balance_now)
	{
		// Account lost balance
		auto it = std::find (accounts_with_balance.begin (), accounts_with_balance.end (), account);
		if (it != accounts_with_balance.end ())
		{
			accounts_with_balance.erase (it);
		}
		balance_lookup.erase (account);
	}
}

nano::uint128_t throughput_account_pool::get_balance (nano::account const & account)
{
	auto it = balances.find (account);
	return (it != balances.end ()) ? it->second : 0;
}

bool throughput_account_pool::has_balance (nano::account const & account)
{
	return balance_lookup.count (account) > 0;
}

size_t throughput_account_pool::accounts_with_balance_count () const
{
	return accounts_with_balance.size ();
}

size_t throughput_account_pool::total_accounts () const
{
	return keys.size ();
}

void throughput_account_pool::set_initial_balance (nano::account const & account, nano::uint128_t balance)
{
	balances[account] = balance;
	if (balance > 0)
	{
		if (balance_lookup.count (account) == 0)
		{
			accounts_with_balance.push_back (account);
			balance_lookup.insert (account);
		}
	}
}

/*
 *
 */

throughput_benchmark::throughput_benchmark (std::shared_ptr<nano::node> node_a, size_t accounts, size_t iterations) :
	node (node_a), num_accounts (accounts), num_iterations (iterations)
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

void throughput_benchmark::run_benchmark ()
{
	std::cout << fmt::format ("Generating {} accounts...\n", num_accounts);
	pool.generate_accounts (num_accounts);

	std::cout << "Setting up genesis distribution...\n";
	setup_genesis_distribution ();

	for (auto iteration = 0; iteration < num_iterations; ++iteration)
	{
		std::cout << fmt::format ("Generating batch {} of random transfers...\n", iteration + 1);
		auto blocks = generate_random_transfers ();

		std::cout << fmt::format ("Processing {} blocks...\n", blocks.size ());
		measure_processing_performance (blocks);

		std::cout << fmt::format ("Iteration {} complete. Processed {} blocks so far.\n", iteration + 1, processed_blocks_count.load ());
		std::cout << "----------------------------------------\n\n";
	}

	print_statistics ();
}

void throughput_benchmark::setup_genesis_distribution ()
{
	// Get genesis balance and latest block
	nano::block_hash genesis_latest (node->latest (nano::dev::genesis_key.pub));
	nano::uint128_t genesis_balance (std::numeric_limits<nano::uint128_t>::max ());

	// Select random account to receive all genesis funds
	nano::account target_account = pool.get_random_account ();
	auto & target_keypair = pool.get_keypair (target_account);

	// Create send block from genesis to target account
	auto send = builder.state ()
				.account (nano::dev::genesis_key.pub)
				.previous (genesis_latest)
				.representative (nano::dev::genesis_key.pub)
				.balance (0)
				.link (target_account)
				.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
				.work (0)
				.build ();

	// Create open block for target account
	auto open = builder.state ()
				.account (target_account)
				.previous (0)
				.representative (target_account)
				.balance (genesis_balance)
				.link (send->hash ())
				.sign (target_keypair.prv, target_keypair.pub)
				.work (0)
				.build ();

	// Process blocks
	auto result1 = node->process_local (send);
	release_assert (result1 && result1 == nano::block_status::progress, to_string (*result1));
	auto result2 = node->process_local (open);
	release_assert (result2 && result2 == nano::block_status::progress, to_string (*result2));

	// Update pool balance tracking
	pool.set_initial_balance (target_account, genesis_balance);

	// Initialize frontier for target account
	frontiers[target_account] = open->hash ();

	std::cout << fmt::format ("Genesis distribution complete. Target account: {}\n", target_account.to_string ());
}

std::deque<std::shared_ptr<nano::block>> throughput_benchmark::generate_random_transfers ()
{
	std::deque<std::shared_ptr<nano::block>> blocks;
	std::random_device rd;
	std::mt19937 gen (rd ());
	std::uniform_int_distribution<uint64_t> amount_dist (1, std::numeric_limits<uint64_t>::max ()); // TODO: Use max uint128_t

	// Generate batch_size number of transfer pairs (send + receive = 2 blocks each)
	size_t batch_size = 50000; // Default batch size
	size_t transfers_generated = 0;

	while (transfers_generated < batch_size / 2) // Divide by 2 since each transfer creates 2 blocks
	{
		if (pool.accounts_with_balance_count () == 0)
		{
			std::cout << "No accounts with balance remaining, stopping...\n";
			break;
		}

		// Get random sender with balance
		nano::account sender = pool.get_random_account_with_balance ();
		auto & sender_keypair = pool.get_keypair (sender);
		nano::uint128_t sender_balance = pool.get_balance (sender);

		if (sender_balance == 0)
			continue;

		// Get random receiver
		nano::account receiver = pool.get_random_account ();
		auto & receiver_keypair = pool.get_keypair (receiver);

		// Random transfer amount (but not more than sender balance)
		nano::uint128_t transfer_amount = std::min (static_cast<nano::uint128_t> (amount_dist (gen)), sender_balance);

		// Get or initialize sender frontier
		nano::block_hash sender_frontier;
		nano::root work_root;
		if (frontiers.count (sender) && frontiers[sender] != 0)
		{
			sender_frontier = frontiers[sender];
			work_root = sender_frontier;
		}
		else
		{
			sender_frontier = 0; // First block for this account
			work_root = sender; // Use account address for first block work
		}

		// Create send block
		nano::uint128_t new_sender_balance = sender_balance - transfer_amount;
		auto send = builder.state ()
					.account (sender)
					.previous (sender_frontier)
					.representative (sender)
					.balance (new_sender_balance)
					.link (receiver)
					.sign (sender_keypair.prv, sender_keypair.pub)
					.work (0)
					.build ();

		blocks.push_back (send);
		frontiers[sender] = send->hash ();
		pool.update_balance (sender, new_sender_balance);

		// Create receive block
		nano::uint128_t receiver_balance = pool.get_balance (receiver);
		nano::uint128_t new_receiver_balance = receiver_balance + transfer_amount;

		nano::block_hash receiver_frontier;
		nano::root receiver_work_root;
		if (frontiers.count (receiver) && frontiers[receiver] != 0)
		{
			receiver_frontier = frontiers[receiver];
			receiver_work_root = receiver_frontier;
		}
		else
		{
			receiver_frontier = 0; // First block for this account (open block)
			receiver_work_root = receiver; // Use account address for first block work
		}

		auto receive = builder.state ()
					   .account (receiver)
					   .previous (receiver_frontier)
					   .representative (receiver)
					   .balance (new_receiver_balance)
					   .link (send->hash ())
					   .sign (receiver_keypair.prv, receiver_keypair.pub)
					   .work (0)
					   .build ();

		blocks.push_back (receive);
		frontiers[receiver] = receive->hash ();
		pool.update_balance (receiver, new_receiver_balance);

		transfers_generated++;

		if (transfers_generated % 10000 == 0)
		{
			std::cout << fmt::format ("Generated {} transfer pairs, {} accounts with balance\n",
			transfers_generated, pool.accounts_with_balance_count ());
		}
	}

	std::cout << fmt::format ("Generated {} total blocks\n", blocks.size ());

	return blocks;
}

void throughput_benchmark::measure_processing_performance (std::deque<std::shared_ptr<nano::block>> & blocks)
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

		// node->process_active (block);

		// auto result = node->process_local (block);
		// if (!result || result.value () != nano::block_status::progress)
		// {
		// 	std::cerr << fmt::format ("Failed to process block: {}\n", block->hash ().to_string ());
		// 	continue;
		// }

		bool added = node->block_processor.add (block, nano::block_source::test);
		release_assert (added, "failed to add block to processor");
	}

	// Wait for processing to complete
	nano::interval progress_interval;
	while (true)
	{
		{
			auto lock = current_blocks.lock ();
			if (lock->empty ())
			{
				break;
			}
			if (progress_interval.elapse (3s))
			{
				std::cout << fmt::format ("{} blocks remaining to process\n", lock->size ());
			}
		}

		std::this_thread::sleep_for (10ms);
	}

	auto const time_end = std::chrono::high_resolution_clock::now ();
	auto const time_us = std::chrono::duration_cast<std::chrono::microseconds> (time_end - time_begin).count ();

	std::cout << fmt::format ("Processing completed in {:.2f}s ({} blocks/sec)\n",
	time_us / 1000000.0, total_blocks * 1000000 / time_us);
}

void throughput_benchmark::print_statistics ()
{
	std::cout << "\n=== Benchmark Statistics ===\n";
	std::cout << fmt::format ("Total accounts: {}\n", pool.total_accounts ());
	std::cout << fmt::format ("Accounts with balance: {}\n", pool.accounts_with_balance_count ());
	std::cout << fmt::format ("Total blocks processed: {}\n", processed_blocks_count.load ());
	std::cout << fmt::format ("Account utilization: {:.1f}%\n",
	100.0 * pool.accounts_with_balance_count () / pool.total_accounts ());
}

/*
 *
 */

void run_throughput_benchmark (boost::program_options::variables_map const & vm, std::filesystem::path const & data_path)
{
	nano::network_constants::set_active_network ("dev");

	// Parse configuration
	size_t num_accounts = 50000;
	size_t num_iterations = 10;

	if (vm.count ("accounts"))
	{
		num_accounts = std::stoull (vm["accounts"].as<std::string> ());
	}
	if (vm.count ("iterations"))
	{
		num_iterations = std::stoull (vm["iterations"].as<std::string> ());
	}

	std::cout << fmt::format ("Starting throughput benchmark with {} accounts, {} iterations\n",
	num_accounts, num_iterations);

	// Setup node (similar to debug_profile_process)
	nano::node_flags node_flags;
	nano::update_flags (node_flags, vm);

	auto io_ctx = std::make_shared<boost::asio::io_context> ();
	nano::work_pool work_pool{ nano::dev::network_params.network, std::numeric_limits<unsigned>::max () };

	// Node configuration
	nano::node_config node_config;
	node_config.peering_port = 0; // Use random available port
	node_config.max_backlog = 0; // Disable bounded backlog
	node_config.block_processor.max_system_queue = std::numeric_limits<size_t>::max (); // Unlimited queue size
	node_config.max_unchecked_blocks = 1024 * 1024;

	auto node = std::make_shared<nano::node> (io_ctx, nano::unique_path (), node_config, work_pool, node_flags);
	node->start ();
	nano::thread_runner runner (io_ctx, nano::default_logger (), node->config.io_threads);

	// Wait for node to be ready
	std::this_thread::sleep_for (500ms);

	// Run benchmark
	throughput_benchmark benchmark{ node, num_accounts, num_iterations };
	benchmark.run_benchmark ();

	node->stop ();
}
}