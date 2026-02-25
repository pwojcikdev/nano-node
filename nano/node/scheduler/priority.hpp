#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/observer_set.hpp>
#include <nano/lib/stored_block.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/scheduler/bucket.hpp>
#include <nano/node/scheduler/priority_pool.hpp>

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace nano::scheduler
{
class priority_config
{
public:
	nano::error deserialize (nano::tomlconfig &);
	nano::error serialize (nano::tomlconfig &) const;

public:
	bool enable{ true };

	// Pool configuration
	size_t max_blocks{ 1024 * 64 }; // Total shared pool size across all buckets
	size_t reserved_blocks{ 1024 * 8 }; // Reserved blocks per bucket

	// Election configuration
	size_t reserved_elections{ 100 }; // Guaranteed election slots per bucket
	size_t max_elections{ 150 }; // Maximum election slots per bucket when AEC has space

	std::chrono::milliseconds cleanup_interval{ 100 };
};

class priority final
{
public:
	priority (nano::node_config &, nano::node &, nano::ledger &, nano::ledger_notifications &, nano::bucketing &, nano::active_elections &, nano::cementing_set &, nano::stats &, nano::logger &);
	~priority ();

	void start ();
	void stop ();

	/**
	 * Activates the first unconfirmed block of \p account
	 * @return true if account was activated
	 */
	bool activate (nano::secure::transaction const &, nano::account const &);
	bool activate (nano::secure::transaction const &, nano::account const &, nano::account_info const &, nano::confirmation_height_info const &);
	bool activate_successors (nano::secure::transaction const &, nano::block const &);

	bool contains (nano::block_hash const &) const;
	void notify ();
	size_t size () const;
	bool empty () const;

	size_t pool_size () const;
	size_t pool_size (nano::bucket_index) const;
	size_t election_count (nano::bucket_index) const;

	nano::container_info container_info () const;

public: // Testing
	bool push (nano::stored_block const & block, nano::bucket_index, nano::priority_timestamp);

public: // Events
	// Triggered when blocks are activated (elections started) in the scheduler run loop
	nano::observer_set<std::deque<nano::block_hash>> batch_activated;

private: // Dependencies
	priority_config const & config;
	nano::node & node;
	nano::ledger & ledger;
	nano::ledger_notifications & ledger_notifications;
	nano::bucketing & bucketing;
	nano::active_elections & active;
	nano::cementing_set & cementing_set;
	nano::stats & stats;
	nano::logger & logger;

private:
	void run ();
	void run_cleanup ();
	bool predicate () const;

private:
	std::map<nano::bucket_index, std::unique_ptr<scheduler::bucket>> buckets;
	priority_pool pool;

	bool stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;
	std::thread cleanup_thread;
};
}
