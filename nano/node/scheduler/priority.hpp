#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/scheduler/priority_pool.hpp>

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <queue>
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
	std::size_t max_blocks{ 1024 * 64 };  // Total shared pool size across all buckets
	std::size_t reserved_blocks{ 1024 * 8 }; // Reserved blocks per bucket
	
	// Election configuration  
	std::size_t reserved_elections{ 100 }; // Guaranteed election slots per bucket
	std::size_t max_elections{ 150 }; // Maximum election slots per bucket when AEC has space
};

class priority final
{
public:
	priority (nano::node_config &, nano::node &, nano::ledger &, nano::ledger_notifications &, nano::bucketing &, nano::active_elections &, nano::cementing_set &, nano::stats &, nano::logger &);
	~priority ();

	void start ();
	void stop ();

	/**
	 * Activates the first unconfirmed block of \p account_a
	 * @return true if account was activated
	 */
	bool activate (nano::secure::transaction const &, nano::account const &);
	bool activate (nano::secure::transaction const &, nano::account const &, nano::account_info const &, nano::confirmation_height_info const &);
	bool activate_successors (nano::secure::transaction const &, nano::block const &);

	bool contains (nano::block_hash const &) const;

	void notify (int64_t vacancy);
	std::size_t size () const;
	bool empty () const;

	nano::container_info container_info () const;

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
	bool predicate () const;
	bool bucket_activate_predicate (nano::bucket_index, nano::priority_timestamp candidate_timestamp, int64_t aec_vacancy, size_t election_count, nano::active_elections::priority_result const & worst) const;
	bool bucket_overfill_predicate (nano::bucket_index, int64_t aec_vacancy, size_t election_count) const;

	void run ();
	bool run_one (nano::unique_lock<nano::mutex> &);

private:
	priority_pool pool;

	bool stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;
};
}
