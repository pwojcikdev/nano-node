#pragma once

#include <nano/lib/blocks.hpp>
#include <nano/node/node.hpp>
#include <nano/secure/common.hpp>

#include <boost/program_options.hpp>

#include <memory>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nano
{
class throughput_account_pool
{
private:
	std::vector<nano::keypair> keys;
	std::unordered_map<nano::account, nano::keypair> account_to_keypair;
	std::unordered_map<nano::account, nano::uint128_t> balances;
	std::vector<nano::account> accounts_with_balance;
	std::unordered_set<nano::account> balance_lookup;
	std::random_device rd;
	std::mt19937 gen;

public:
	throughput_account_pool ();

	void generate_accounts (size_t count);
	nano::account get_random_account_with_balance ();
	nano::account get_random_account ();
	nano::keypair const & get_keypair (nano::account const & account);
	void update_balance (nano::account const & account, nano::uint128_t new_balance);
	nano::uint128_t get_balance (nano::account const & account);
	bool has_balance (nano::account const & account);
	size_t accounts_with_balance_count () const;
	size_t total_accounts () const;
	void set_initial_balance (nano::account const & account, nano::uint128_t balance);
};

class throughput_benchmark
{
private:
	throughput_account_pool pool;
	std::shared_ptr<nano::node> node;
	size_t num_accounts;
	size_t num_iterations;
	nano::block_builder builder;
	std::unordered_map<nano::account, nano::block_hash> frontiers;

	// Blocks currently being processed
	nano::locked<std::unordered_set<nano::block_hash>> current_blocks;
	
	// Independently tracked processed blocks count
	std::atomic<size_t> processed_blocks_count{ 0 };

public:
	throughput_benchmark (std::shared_ptr<nano::node> node_a, size_t accounts, size_t iterations);

	void run_benchmark ();
	void setup_genesis_distribution ();
	std::deque<std::shared_ptr<nano::block>> generate_random_transfers ();
	void measure_processing_performance (std::deque<std::shared_ptr<nano::block>> & blocks);
	void print_statistics ();
};

void run_throughput_benchmark (boost::program_options::variables_map const & vm, std::filesystem::path const & data_path);
}