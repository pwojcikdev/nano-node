#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/lib/vote.hpp>
#include <nano/node/network.hpp>
#include <nano/node/vote_spacing.hpp>
#include <nano/node/voting/final_vote_generator.hpp>
#include <nano/node/voting/vote_criteria.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/ledger/final_vote.hpp>

#include <chrono>

nano::final_vote_generator::final_vote_generator (final_vote_generator_config const & config_a, nano::ledger & ledger_a, nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::network & network_a, nano::network_params & network_params_a, nano::stats & stats_a, nano::logger & logger_a, std::shared_ptr<nano::transport::channel> inproc_channel_a) :
	config (config_a),
	ledger (ledger_a),
	stats (stats_a),
	logger (logger_a),
	spacing_impl{ std::make_unique<nano::vote_spacing> (network_params_a.voting.delay) },
	spacing{ *spacing_impl },
	broadcaster (wallets_a, vote_processor_a, history_a, spacing, network_a, stats_a, std::move (inproc_channel_a)),
	vote_generation_queue{ stats, nano::stat::type::vote_generator_final, nano::thread_role::name::voting_final, /* single threaded */ 1, config.max_queue, config.batch_size }
{
	vote_generation_queue.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};
}

nano::final_vote_generator::~final_vote_generator ()
{
	debug_assert (stopped);
	debug_assert (!thread.joinable ());
}

void nano::final_vote_generator::add (nano::root const & root, nano::block_hash const & hash)
{
	vote_generation_queue.add (std::make_pair (root, hash));
}

void nano::final_vote_generator::process_batch (std::deque<queue_entry_t> & batch)
{
	std::deque<candidate_t> verified;

	auto transaction = ledger.tx_begin_write (nano::store::writer::voting_final);
	for (auto & [root, hash] : batch)
	{
		transaction.refresh_if_needed ();

		if (nano::vote_criteria::should_vote_final (transaction, ledger, root, hash))
		{
			verified.emplace_back (root, hash);
		}
	}
	// Commit write transaction (scope exit)

	if (!verified.empty ())
	{
		nano::unique_lock<nano::mutex> lock{ mutex };
		candidates.insert (candidates.end (), verified.begin (), verified.end ());
		if (candidates.size () >= nano::network::confirm_ack_hashes_max)
		{
			lock.unlock ();
			condition.notify_all ();
		}
	}
}

void nano::final_vote_generator::start ()
{
	debug_assert (!thread.joinable ());
	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::voting_final);
		run ();
	});
	vote_generation_queue.start ();
}

void nano::final_vote_generator::stop ()
{
	vote_generation_queue.stop ();
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();

	if (thread.joinable ())
	{
		thread.join ();
	}
}

void nano::final_vote_generator::broadcast (nano::unique_lock<nano::mutex> & lock_a)
{
	debug_assert (lock_a.owns_lock ());

	auto [hashes, roots] = broadcaster.collect_votable_batch (candidates, nano::stat::type::vote_generator_final);
	if (!hashes.empty ())
	{
		lock_a.unlock ();
		broadcaster.vote (hashes, roots, /* is_final */ true, [this] (auto const & generated_vote) {
			stats.inc (nano::stat::type::vote_generator_final, nano::stat::detail::generator_broadcasts);
			stats.sample (nano::stat::sample::vote_generator_final_hashes, generated_vote->hashes.size (), { 0, nano::network::confirm_ack_hashes_max });
			broadcaster.broadcast (generated_vote, nano::stat::type::vote_generator_final);
		});
		lock_a.lock ();
	}
}

bool nano::final_vote_generator::broadcast_predicate () const
{
	debug_assert (!mutex.try_lock ());

	if (candidates.size () >= nano::network::confirm_ack_hashes_max)
	{
		return true;
	}
	if (!candidates.empty () && std::chrono::steady_clock::now () > next_broadcast)
	{
		return true;
	}
	return false;
}

void nano::final_vote_generator::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		condition.wait_for (lock, config.delay, [this] () {
			return stopped || broadcast_predicate ();
		});

		if (stopped)
		{
			return;
		}

		if (broadcast_predicate ())
		{
			stats.inc (nano::stat::type::vote_generator_final, nano::stat::detail::loop);

			if (candidates.size () > nano::queue_warning_threshold () && log_interval.elapse (15s))
			{
				logger.info (nano::log::type::vote_generator_final, "{} candidates in processing queue", candidates.size ());
			}

			broadcast (lock);
			next_broadcast = std::chrono::steady_clock::now () + config.delay;
		}
	}
}

nano::container_info nano::final_vote_generator::container_info () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	nano::container_info info;
	info.put ("candidates", candidates.size ());
	info.add ("queue", vote_generation_queue.container_info ());
	return info;
}

/*
 * final_vote_generator_config
 */

nano::error nano::final_vote_generator_config::serialize (nano::tomlconfig & toml) const
{
	toml.put ("max_queue", max_queue, "Maximum number of entries in the final vote generation queue. \ntype:uint64");
	toml.put ("batch_size", batch_size, "Maximum number of entries to process in a single batch. \ntype:uint64");
	toml.put ("delay", delay.count (), "Delay before final votes are sent to allow for efficient bundling of hashes in votes. \ntype:milliseconds");

	return toml.get_error ();
}

nano::error nano::final_vote_generator_config::deserialize (nano::tomlconfig & toml)
{
	toml.get ("max_queue", max_queue);
	toml.get ("batch_size", batch_size);
	toml.get_duration ("delay", delay);

	return toml.get_error ();
}
