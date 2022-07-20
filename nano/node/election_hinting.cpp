#include <nano/lib/threading.hpp>
#include <nano/node/active_transactions.hpp>
#include <nano/node/election_hinting.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/online_reps.hpp>
#include <nano/node/vote_cache.hpp>
#include <nano/secure/store.hpp>

nano::election_hinting::election_hinting (nano::node & node_a, const config & config_a, nano::vote_cache & vote_cache_a, nano::active_transactions & active_a, nano::store & store_a, nano::online_reps & online_reps_a) :
	stopped{ false },
	node_m{ node_a },
	config_m{ config_a },
	vote_cache_m{ vote_cache_a },
	active_m{ active_a },
	store_m{ store_a },
	online_reps_m{ online_reps_a },
	thread{ [this] () { run (); } }
{
}

nano::election_hinting::~election_hinting ()
{
	stop ();
	thread.join ();
}

void nano::election_hinting::stop ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	stopped = true;
	notify ();
}

void nano::election_hinting::flush ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	condition.wait (lock, [this] () {
		return stopped || empty () || active_m.vacancy () <= 0;
	});
}

bool nano::election_hinting::empty () const
{
	return vote_cache_m.queue_empty ();
}

std::size_t nano::election_hinting::size () const
{
	return vote_cache_m.queue_size ();
}

void nano::election_hinting::notify ()
{
	condition.notify_all ();
}

bool nano::election_hinting::predicate (nano::uint128_t minimum_tally) const
{
	// Check if there is space in AEC for starting a new hinted election
	if (active_m.vacancy_hinted () > 0)
	{
		// Check if we have a potential block in hinted queue that reaches minimum voting weight threshold
		if (vote_cache_m.peek (minimum_tally))
		{
			return true;
		}
	}
	return false;
}

bool nano::election_hinting::run_one (nano::uint128_t minimum_tally)
{
	if (auto top = vote_cache_m.pop (minimum_tally); top)
	{
		auto hash = top->hash;
		auto transaction (store_m.tx_begin_read ());
		auto block = store_m.block.get (transaction, hash);
		if (block != nullptr)
		{
			debug_assert (block->hash () == hash);
			if (!node_m.block_confirmed_or_being_confirmed (transaction, hash))
			{
				auto result = active_m.insert_hinted (block);
				if (result.election)
				{
					result.election->transition_active ();
				}
				return result.inserted;
			}
		}
		else
		{
			// Missing block in ledger to start an election, request bootstrapping it
			node_m.bootstrap_block (transaction, hash);
		}
	}
	return false;
}

void nano::election_hinting::run ()
{
	nano::thread_role::set (nano::thread_role::name::election_hinting);
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		// Periodically wakeup for condition checking as we do not call notify when new votes arrive in cache as that happens too often (we only notify on aec vaccancy)
		condition.wait_for (lock, std::chrono::seconds (1), [this] () {
			return stopped || predicate (tally_threshold ());
		});
		debug_assert ((std::this_thread::yield (), true)); // Introduce some random delay in debug builds
		if (!stopped)
		{
			const auto minimum_tally = tally_threshold ();
			if (predicate (minimum_tally))
			{
				lock.unlock ();
				run_one (minimum_tally);
				notify ();
				lock.lock ();
			}
		}
	}
}

nano::uint128_t nano::election_hinting::tally_threshold () const
{
	const auto min_tally = (online_reps_m.trended () / 100) * config_m.election_hint_weight_percent;
	return min_tally;
}
