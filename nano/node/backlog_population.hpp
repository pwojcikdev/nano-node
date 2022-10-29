#pragma once

#include <nano/lib/locks.hpp>

#include <atomic>
#include <condition_variable>
#include <thread>

namespace nano
{
class store;
class election_scheduler;
class stats;

class backlog_population final
{
public:
	struct config
	{
		bool ongoing_backlog_population_enabled;
		unsigned int delay_between_runs_seconds;
	};

	backlog_population (config, nano::store &, nano::election_scheduler &, nano::stat &);
	~backlog_population ();

	void start ();
	void stop ();
	void trigger ();

	/** Other components call this to notify us about external changes, so we can check our predicate. */
	void notify ();

private:
	void run ();
	bool predicate () const;
	/**
	 * Scans ledger for unconfirmed account chains and activates them via election scheduler
	 * @return whether there was an overflow of priority queue
	 */
	bool populate_backlog ();

private: // Dependencies
	nano::store & store;
	nano::election_scheduler & scheduler;
	nano::stat & stats;

private:
	const config config_m;

	/**
	 * This is a manual trigger, the ongoing backlog population does not use this.
	 * It can be triggered even when backlog population (frontiers confirmation) is disabled
	 */
	bool triggered{ false };
	/**
	 * TODO: Docs
	 */
	bool overflown{ false };

	std::atomic<bool> stopped{ false };

	nano::condition_variable condition;
	mutable nano::mutex mutex;

	/** Thread that runs the backlog implementation logic. The thread always runs, even if
	 *  backlog population is disabled, so that it can service a manual trigger (e.g. via RPC). */
	std::thread thread;
};
}
