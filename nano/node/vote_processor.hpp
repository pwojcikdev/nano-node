#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/utility.hpp>
#include <nano/secure/common.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace nano
{
class signature_checker;
class active_transactions;
class store;
class node_observers;
class stats;
class node_config;
class logger_mt;
class online_reps;
class rep_crawler;
class ledger;
class network_params;
class node_flags;
class stat;
class transaction;
namespace transport
{
	class channel;
}

class vote_processor final
{
	using entry_t = std::pair<std::shared_ptr<nano::vote>, std::shared_ptr<nano::transport::channel>>;

public:
	explicit vote_processor (nano::signature_checker & checker_a, nano::active_transactions & active_a, nano::node_observers & observers_a, nano::stat & stats_a, nano::node_config & config_a, nano::node_flags & flags_a, nano::logger_mt & logger_a, nano::online_reps & online_reps_a, nano::rep_crawler & rep_crawler_a, nano::ledger & ledger_a, nano::network_params & network_params_a);
	~vote_processor ();

	/** Returns false if the vote was processed */
	bool vote (std::shared_ptr<nano::vote> const &, std::shared_ptr<nano::transport::channel> const &);
	nano::vote_code vote_blocking (std::shared_ptr<nano::vote> const &, std::shared_ptr<nano::transport::channel> const &, bool verified = false);
	void verify_votes (std::deque<entry_t> const &);
	/** Function blocks until either the current queue size (a established flush boundary as it'll continue to increase)
	 * is processed or the queue is empty (end condition or cutoff's guard, as it is positioned ahead) */
	void flush ();
	std::size_t size ();
	bool empty ();
	bool half_full ();
	void calculate_weights ();
	void stop ();
	std::atomic<uint64_t> total_processed{ 0 };

private:
	void start_threads ();
	void process_loop ();
	std::deque<entry_t> get_batch ();

	bool should_process_locked (nano::account representative) const;

private:
	std::size_t const max_votes;

	std::deque<entry_t> votes;

	/** Representatives levels for random early detection */
	std::unordered_set<nano::account> representatives_1;
	std::unordered_set<nano::account> representatives_2;
	std::unordered_set<nano::account> representatives_3;

	nano::condition_variable condition;
	nano::mutex mutex{ mutex_identifier (mutexes::vote_processor) };
	std::atomic<bool> stopped;
	std::vector<std::thread> processing_threads;

private: // Dependencies
	nano::signature_checker & checker;
	nano::active_transactions & active;
	nano::node_observers & observers;
	nano::stat & stats;
	nano::node_config & config;
	nano::logger_mt & logger;
	nano::online_reps & online_reps;
	nano::rep_crawler & rep_crawler;
	nano::ledger & ledger;
	nano::network_params & network_params;

	friend std::unique_ptr<container_info_component> collect_container_info (vote_processor & vote_processor, std::string const & name);
	friend class vote_processor_weights_Test;
};

std::unique_ptr<container_info_component> collect_container_info (vote_processor & vote_processor, std::string const & name);
}
