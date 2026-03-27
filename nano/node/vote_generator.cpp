#include <nano/lib/blocks.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/lib/vote.hpp>
#include <nano/node/local_vote_history.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/inproc.hpp>
#include <nano/node/vote_generator.hpp>
#include <nano/node/vote_processor.hpp>
#include <nano/node/vote_spacing.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/voting_policy.hpp>

#include <chrono>

/*
 * vote_generator
 */

nano::vote_generator::vote_generator (vote_generator_config const & config_a, nano::node & node_a, nano::voting_policy & policy_a, nano::ledger & ledger_a, nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::network & network_a, nano::stats & stats_a, nano::logger & logger_a, bool is_final_a, std::shared_ptr<nano::transport::channel> inproc_channel_a) :
	config (config_a),
	node (node_a),
	policy (policy_a),
	ledger (ledger_a),
	wallets (wallets_a),
	vote_processor (vote_processor_a),
	history (history_a),
	spacing_impl{ std::make_unique<nano::vote_spacing> (node_a.network_params.voting.delay) },
	spacing{ *spacing_impl },
	network (network_a),
	stats (stats_a),
	logger (logger_a),
	is_final (is_final_a),
	inproc_channel{ inproc_channel_a },
	vote_generation_queue{ stats, nano::stat::type::vote_generator, is_final ? nano::thread_role::name::voting_final : nano::thread_role::name::voting, /* single threaded */ 1, config.max_queue, config.batch_size }
{
	vote_generation_queue.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};
}

nano::vote_generator::~vote_generator ()
{
	debug_assert (stopped);
	debug_assert (!thread.joinable ());
}

void nano::vote_generator::start ()
{
	debug_assert (!thread.joinable ());
	thread = std::thread ([this] () {
		nano::thread_role::set (is_final ? nano::thread_role::name::voting_final : nano::thread_role::name::voting);
		run ();
	});
	vote_generation_queue.start ();
}

void nano::vote_generator::stop ()
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

void nano::vote_generator::add (const root & root, const block_hash & hash)
{
	vote_generation_queue.add (std::make_pair (root, hash));
}

void nano::vote_generator::process_batch (std::deque<queue_entry_t> & batch)
{
	std::deque<nano::vote_permit> verified;

	if (is_final)
	{
		auto transaction = ledger.tx_begin_write (nano::store::writer::voting_final);
		for (auto & [root, hash] : batch)
		{
			transaction.refresh_if_needed ();
			auto block = ledger.any.block_get (transaction, hash);
			if (block)
			{
				if (auto permit = policy.vote_final (transaction, *block))
				{
					verified.push_back (*permit);
				}
			}
		}
		// Commit write transaction
	}
	else
	{
		auto transaction = ledger.tx_begin_read ();
		for (auto & [root, hash] : batch)
		{
			transaction.refresh_if_needed ();
			auto block = ledger.any.block_get (transaction, hash);
			if (block)
			{
				if (auto permit = policy.vote (transaction, *block))
				{
					verified.push_back (*permit);
				}
			}
		}
	}

	// Submit verified candidates to the main processing thread
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

void nano::vote_generator::broadcast (nano::unique_lock<nano::mutex> & lock_a)
{
	debug_assert (lock_a.owns_lock ());

	std::vector<nano::vote_permit> permits;
	permits.reserve (nano::network::confirm_ack_hashes_max);
	std::vector<nano::root> seen_roots;
	seen_roots.reserve (nano::network::confirm_ack_hashes_max);
	while (!candidates.empty () && permits.size () < nano::network::confirm_ack_hashes_max)
	{
		auto permit = std::move (candidates.front ());
		candidates.pop_front ();
		if (std::find (seen_roots.begin (), seen_roots.end (), permit.root ()) == seen_roots.end ())
		{
			if (spacing.votable (permit.root (), permit.hash ()))
			{
				seen_roots.push_back (permit.root ());
				permits.push_back (std::move (permit));
			}
			else
			{
				stats.inc (stat_type (), nano::stat::detail::generator_spacing);
			}
		}
	}
	if (!permits.empty ())
	{
		lock_a.unlock ();
		auto votes = policy.sign (is_final ? nano::vote_type::final : nano::vote_type::normal, permits, wallets.signer ());
		for (auto const & vote : votes)
		{
			for (auto const & permit : permits)
			{
				history.add (permit.root (), permit.hash (), vote);
				spacing.flag (permit.root (), permit.hash ());
			}
			stats.inc (stat_type (), nano::stat::detail::generator_broadcasts);
			stats.sample (is_final ? nano::stat::sample::vote_generator_final_hashes : nano::stat::sample::vote_generator_hashes, vote->hashes.size (), { 0, nano::network::confirm_ack_hashes_max });
			broadcast_action (vote);
		}
		lock_a.lock ();
	}
}

void nano::vote_generator::broadcast_action (std::shared_ptr<nano::vote> const & vote_a) const
{
	vote_processor.vote (vote_a, inproc_channel);

	auto sent_pr = network.flood_vote_pr (vote_a);
	auto sent_non_pr = network.flood_vote_non_pr (vote_a, 2.0f);

	stats.add (stat_type (), nano::stat::detail::sent_pr, sent_pr);
	stats.add (stat_type (), nano::stat::detail::sent_non_pr, sent_non_pr);
}

void nano::vote_generator::run ()
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
			stats.inc (stat_type (), nano::stat::detail::loop);

			if (candidates.size () > nano::queue_warning_threshold () && log_interval.elapse (15s))
			{
				logger.info (log_type (), "{} candidates in processing queue", candidates.size ());
			}

			broadcast (lock);
			next_broadcast = std::chrono::steady_clock::now () + config.delay;
		}
	}
}

bool nano::vote_generator::broadcast_predicate () const
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

nano::container_info nano::vote_generator::container_info () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	nano::container_info info;
	info.put ("candidates", candidates.size ());
	info.add ("queue", vote_generation_queue.container_info ());
	return info;
}

nano::stat::type nano::vote_generator::stat_type () const
{
	return is_final ? nano::stat::type::vote_generator_final : nano::stat::type::vote_generator;
}

nano::log::type nano::vote_generator::log_type () const
{
	return is_final ? nano::log::type::vote_generator_final : nano::log::type::vote_generator;
}

/*
 * vote_generator_config
 */

nano::error nano::vote_generator_config::serialize (nano::tomlconfig & toml) const
{
	toml.put ("max_queue", max_queue, "Maximum number of entries in the vote generation queue. \ntype:uint64");
	toml.put ("batch_size", batch_size, "Maximum number of entries to process in a single batch. \ntype:uint64");
	toml.put ("delay", delay.count (), "Delay before votes are sent to allow for efficient bundling of hashes in votes. \ntype:milliseconds");

	return toml.get_error ();
}

nano::error nano::vote_generator_config::deserialize (nano::tomlconfig & toml)
{
	toml.get ("max_queue", max_queue);
	toml.get ("batch_size", batch_size);
	toml.get_duration ("delay", delay);

	return toml.get_error ();
}

/*
 * vote_broadcast_index
 */

nano::vote_broadcast_index::vote_broadcast_index (size_t max_size) :
	max_size{ max_size }
{
}

bool nano::vote_broadcast_index::push (nano::qualified_root const & root, nano::vote_permit permit)
{
	auto & by_root = entries.get<tag_root> ();
	if (auto existing = by_root.find (root); existing != by_root.end ())
	{
		if (existing->permit.hash () == permit.hash ())
		{
			return false; // Duplicate
		}
		// Conflict: different hash for same root, replace old entry
		by_root.erase (existing);
	}
	if (entries.size () >= max_size)
	{
		return false;
	}
	auto & sequenced = entries.get<tag_sequenced> ();
	auto [it, inserted] = sequenced.push_back ({ root, permit });
	return inserted;
}

bool nano::vote_broadcast_index::erase (nano::qualified_root const & root)
{
	auto & by_root = entries.get<tag_root> ();
	return by_root.erase (root) > 0;
}

std::deque<nano::vote_permit> nano::vote_broadcast_index::next_batch (size_t count)
{
	auto & sequenced = entries.get<tag_sequenced> ();
	std::deque<nano::vote_permit> batch;
	while (!sequenced.empty () && batch.size () < count)
	{
		batch.push_back (sequenced.front ().permit);
		sequenced.pop_front ();
	}
	return batch;
}

size_t nano::vote_broadcast_index::size () const
{
	return entries.size ();
}

bool nano::vote_broadcast_index::empty () const
{
	return entries.empty ();
}

/*
 * vote_generator_index
 */

nano::vote_generator_index::vote_generator_index (size_t max_size_per_bucket)
{
	queue.max_size_query = [max_size_per_bucket] (auto const &) {
		return max_size_per_bucket;
	};
	queue.priority_query = [] (auto const &) {
		return size_t{ 1 };
	};
}

bool nano::vote_generator_index::push (nano::qualified_root const & root, nano::block_hash const & hash, nano::bucket_index bucket)
{
	if (auto existing = dedup.find (root); existing != dedup.end ())
	{
		if (existing->second == hash)
		{
			return false; // Duplicate
		}
		else
		{
			// Different hash for same root
			// Update dedup (old becomes stale)
			existing->second = hash;
			bool added = queue.push ({ root, hash }, { bucket });
			return added;
		}
	}
	else
	{
		// New root
		if (queue.push ({ root, hash }, { bucket }))
		{
			dedup.emplace (root, hash);
			return true;
		}
	}
	return false; // Queue full
}

auto nano::vote_generator_index::next_batch (size_t count) -> std::deque<entry>
{
	std::deque<entry> result;
	while (result.size () < count && !queue.empty ())
	{
		auto [item, origin] = queue.next ();
		auto const & [root, hash] = item;

		// Check if the item is still valid (not stale from a later replacement)
		auto existing = dedup.find (root);
		if (existing != dedup.end () && existing->second == hash)
		{
			dedup.erase (existing);
			result.push_back (item);
		}
		// Otherwise stale — skip
	}
	return result;
}

size_t nano::vote_generator_index::size () const
{
	return dedup.size ();
}

bool nano::vote_generator_index::empty () const
{
	return dedup.empty ();
}