#pragma once

#include <nano/lib/interval.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/processing_queue.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/voting/vote_broadcaster.hpp>

#include <condition_variable>
#include <deque>
#include <thread>

namespace nano
{
class final_vote_generator_config final
{
public:
	nano::error serialize (nano::tomlconfig & toml) const;
	nano::error deserialize (nano::tomlconfig & toml);

public:
	size_t max_queue{ 1024 * 32 };
	size_t batch_size{ 256 };
	std::chrono::milliseconds delay{ 100ms };
};

class final_vote_generator final
{
public:
	using candidate_t = std::pair<nano::root, nano::block_hash>;

	final_vote_generator (final_vote_generator_config const &, nano::ledger &, nano::wallets &, nano::vote_processor &, nano::local_vote_history &, nano::network &, nano::network_params &, nano::stats &, nano::logger &, std::shared_ptr<nano::transport::channel> inproc_channel);
	~final_vote_generator ();

	/** Queue items for final vote generation and broadcast */
	void add (nano::root const &, nano::block_hash const &);

	void start ();
	void stop ();

	nano::container_info container_info () const;

private:
	using queue_entry_t = std::pair<nano::root, nano::block_hash>;

	void run ();
	void broadcast (nano::unique_lock<nano::mutex> &);
	void process_batch (std::deque<queue_entry_t> & batch);
	bool broadcast_predicate () const;

private: // Dependencies
	final_vote_generator_config const & config;
	nano::ledger & ledger;
	nano::stats & stats;
	nano::logger & logger;
	std::unique_ptr<nano::vote_spacing> spacing_impl;
	nano::vote_spacing & spacing;
	nano::vote_broadcaster broadcaster;

private:
	mutable nano::mutex mutex;
	nano::condition_variable condition;
	std::deque<candidate_t> candidates;
	std::atomic<bool> stopped{ false };
	std::thread thread;
	nano::processing_queue<queue_entry_t> vote_generation_queue;
	std::chrono::steady_clock::time_point next_broadcast{ std::chrono::steady_clock::now () };

	nano::interval log_interval;
};
}
