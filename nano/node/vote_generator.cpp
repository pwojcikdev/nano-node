#include <nano/lib/blocks.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/lib/vote.hpp>
#include <nano/node/local_vote_history.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/inproc.hpp>
#include <nano/node/vote_generator.hpp>
#include <nano/node/vote_processor.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/voting_policy.hpp>

#include <chrono>

/*
 * vote_verifier
 */

nano::vote_verifier::vote_verifier (size_t max_size_per_bucket, size_t batch_size, size_t thread_count, nano::thread_role::name thread_role) :
	batch_size{ batch_size },
	thread_count{ thread_count },
	thread_role{ thread_role },
	index{ max_size_per_bucket }
{
}

nano::vote_verifier::~vote_verifier ()
{
	debug_assert (stopped);
}

void nano::vote_verifier::start ()
{
	debug_assert (process_batch);
	for (size_t i = 0; i < thread_count; ++i)
	{
		threads.emplace_back ([this] () {
			nano::thread_role::set (thread_role);
			run ();
		});
	}
}

void nano::vote_verifier::stop ()
{
	stopped = true;
	condition.notify_all ();
	for (auto & t : threads)
	{
		if (t.joinable ())
		{
			t.join ();
		}
	}
}

bool nano::vote_verifier::push (nano::qualified_root const & root, nano::block_hash const & hash, nano::bucket_index bucket)
{
	bool added = false;
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		added = index.push (root, hash, bucket);
	}
	if (added)
	{
		condition.notify_one ();
	}
	return added;
}

void nano::vote_verifier::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		condition.wait (lock, [this] () {
			return stopped || !index.empty ();
		});
		if (stopped)
		{
			return;
		}
		auto batch = index.next_batch (batch_size);
		lock.unlock ();

		if (!batch.empty ())
		{
			process_batch (std::move (batch));
		}

		lock.lock ();
	}
}

size_t nano::vote_verifier::size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return index.size ();
}

bool nano::vote_verifier::empty () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return index.empty ();
}

nano::container_info nano::vote_verifier::container_info () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	nano::container_info info;
	info.put ("index", index.size ());
	return info;
}

/*
 * vote_broadcaster
 */

nano::vote_broadcaster::vote_broadcaster (size_t max_size, size_t batch_threshold, std::chrono::milliseconds delay, nano::thread_role::name thread_role) :
	batch_threshold{ batch_threshold },
	delay{ delay },
	thread_role{ thread_role },
	index{ max_size },
	next_broadcast{ std::chrono::steady_clock::now () }
{
}

nano::vote_broadcaster::~vote_broadcaster ()
{
	debug_assert (stopped);
}

void nano::vote_broadcaster::start ()
{
	debug_assert (broadcast_batch);
	thread = std::thread ([this] () {
		nano::thread_role::set (thread_role);
		run ();
	});
}

void nano::vote_broadcaster::stop ()
{
	stopped = true;
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

bool nano::vote_broadcaster::push (nano::qualified_root const & root, nano::vote_permit permit)
{
	bool added = false;
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		added = index.push (root, std::move (permit));
	}
	if (added)
	{
		condition.notify_all ();
	}
	return added;
}

void nano::vote_broadcaster::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		condition.wait_for (lock, delay, [this] () {
			return stopped || predicate ();
		});
		if (stopped)
		{
			return;
		}
		if (predicate ())
		{
			auto batch = index.next_batch (batch_threshold);
			lock.unlock ();

			if (!batch.empty ())
			{
				broadcast_batch (std::move (batch));
			}

			lock.lock ();
			next_broadcast = std::chrono::steady_clock::now () + delay;
		}
	}
}

bool nano::vote_broadcaster::predicate () const
{
	if (index.size () >= batch_threshold)
	{
		return true;
	}
	if (!index.empty () && std::chrono::steady_clock::now () > next_broadcast)
	{
		return true;
	}
	return false;
}

size_t nano::vote_broadcaster::size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return index.size ();
}

bool nano::vote_broadcaster::empty () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return index.empty ();
}

nano::container_info nano::vote_broadcaster::container_info () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	nano::container_info info;
	info.put ("index", index.size ());
	return info;
}

/*
 * vote_generator
 */

nano::vote_generator::vote_generator (vote_generator_config const & config_a, nano::voting_policy & policy_a, nano::ledger & ledger_a, nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::network & network_a, nano::stats & stats_a, nano::logger & logger_a, nano::voting_constants const & voting_constants_a, std::shared_ptr<nano::transport::channel> inproc_channel_a) :
	config{ config_a },
	policy{ policy_a },
	ledger{ ledger_a },
	wallets{ wallets_a },
	vote_processor{ vote_processor_a },
	history{ history_a },
	network{ network_a },
	stats{ stats_a },
	logger{ logger_a },
	inproc_channel{ inproc_channel_a },
	normal_verifier{ config_a.max_queue, config_a.batch_size, config_a.normal_threads, nano::thread_role::name::voting_normal_processing },
	normal_broadcaster{ config_a.max_queue, nano::network::confirm_ack_hashes_max, config_a.delay, nano::thread_role::name::voting_normal_broadcast },
	normal_spacing{ voting_constants_a.delay },
	final_verifier{ config_a.max_queue, config_a.batch_size, /* single threaded */ 1, nano::thread_role::name::voting_final_processing },
	final_broadcaster{ config_a.max_queue, nano::network::confirm_ack_hashes_max, config_a.delay, nano::thread_role::name::voting_final_broadcast },
	final_spacing{ voting_constants_a.delay }
{
	normal_verifier.process_batch = [this] (auto batch) { process_normal (std::move (batch)); };
	final_verifier.process_batch = [this] (auto batch) { process_final (std::move (batch)); };
	normal_broadcaster.broadcast_batch = [this] (auto batch) { broadcast_normal (std::move (batch)); };
	final_broadcaster.broadcast_batch = [this] (auto batch) { broadcast_final (std::move (batch)); };
}

nano::vote_generator::~vote_generator () = default;

void nano::vote_generator::process_normal (std::deque<vote_verifier::entry> batch)
{
	std::deque<nano::vote_permit> verified;
	auto transaction = ledger.tx_begin_read ();
	for (auto & [qroot, hash] : batch)
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
	for (auto & p : verified)
	{
		normal_broadcaster.push (p.qualified_root (), std::move (p));
	}
}

void nano::vote_generator::process_final (std::deque<vote_verifier::entry> batch)
{
	std::deque<nano::vote_permit> verified;
	auto transaction = ledger.tx_begin_write (nano::store::writer::voting_final);
	for (auto & [qroot, hash] : batch)
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
	for (auto & p : verified)
	{
		final_broadcaster.push (p.qualified_root (), std::move (p));
	}
}

void nano::vote_generator::broadcast_normal (std::deque<nano::vote_permit> batch)
{
	std::vector<nano::vote_permit> permits;
	for (auto & p : batch)
	{
		if (normal_spacing.votable (p.root (), p.hash ()))
		{
			permits.push_back (std::move (p));
		}
		else
		{
			stats.inc (nano::stat::type::vote_generator, nano::stat::detail::generator_spacing);
		}
	}
	if (permits.empty ())
	{
		return;
	}
	auto votes = policy.sign (nano::vote_type::normal, permits, wallets.signer ());
	for (auto const & vote : votes)
	{
		for (auto const & p : permits)
		{
			history.add (p.root (), p.hash (), vote);
			normal_spacing.flag (p.root (), p.hash ());
		}
		stats.inc (nano::stat::type::vote_generator, nano::stat::detail::generator_broadcasts);
		stats.sample (nano::stat::sample::vote_generator_hashes, vote->hashes.size (), { 0, nano::network::confirm_ack_hashes_max });
		broadcast_vote (vote, nano::transport::traffic_type::vote_normal);
	}
}

void nano::vote_generator::broadcast_final (std::deque<nano::vote_permit> batch)
{
	std::vector<nano::vote_permit> permits;
	for (auto & p : batch)
	{
		if (final_spacing.votable (p.root (), p.hash ()))
		{
			permits.push_back (std::move (p));
		}
		else
		{
			stats.inc (nano::stat::type::vote_generator_final, nano::stat::detail::generator_spacing);
		}
	}
	if (permits.empty ())
	{
		return;
	}
	auto votes = policy.sign (nano::vote_type::final, permits, wallets.signer ());
	for (auto const & vote : votes)
	{
		for (auto const & p : permits)
		{
			history.add (p.root (), p.hash (), vote);
			final_spacing.flag (p.root (), p.hash ());
		}
		stats.inc (nano::stat::type::vote_generator_final, nano::stat::detail::generator_broadcasts);
		stats.sample (nano::stat::sample::vote_generator_final_hashes, vote->hashes.size (), { 0, nano::network::confirm_ack_hashes_max });
		broadcast_vote (vote, nano::transport::traffic_type::vote_final);
	}
}

void nano::vote_generator::start ()
{
	normal_verifier.start ();
	final_verifier.start ();
	normal_broadcaster.start ();
	final_broadcaster.start ();
}

void nano::vote_generator::stop ()
{
	normal_verifier.stop ();
	final_verifier.stop ();
	normal_broadcaster.stop ();
	final_broadcaster.stop ();
}

void nano::vote_generator::vote (nano::qualified_root const & root, nano::block_hash const & hash, nano::bucket_index bucket, nano::vote_type type)
{
	switch (type)
	{
		case nano::vote_type::normal:
			vote_normal (root, hash, bucket);
			break;
		case nano::vote_type::final:
			vote_final (root, hash, bucket);
			break;
	}
}

void nano::vote_generator::vote_normal (nano::qualified_root const & root, nano::block_hash const & hash, nano::bucket_index bucket)
{
	if (normal_verifier.push (root, hash, bucket))
	{
		stats.inc (nano::stat::type::vote_generator, nano::stat::detail::queue);
	}
	else
	{
		stats.inc (nano::stat::type::vote_generator, nano::stat::detail::overfill);
	}
}

void nano::vote_generator::vote_final (nano::qualified_root const & root, nano::block_hash const & hash, nano::bucket_index bucket)
{
	if (final_verifier.push (root, hash, bucket))
	{
		stats.inc (nano::stat::type::vote_generator_final, nano::stat::detail::queue);
	}
	else
	{
		stats.inc (nano::stat::type::vote_generator_final, nano::stat::detail::overfill);
	}
}

void nano::vote_generator::broadcast_vote (std::shared_ptr<nano::vote> const & vote_a, nano::transport::traffic_type type) const
{
	vote_processor.vote (vote_a, inproc_channel);

	auto sent_pr = network.flood_vote_pr (vote_a, type);
	auto sent_non_pr = network.flood_vote_non_pr (vote_a, 2.0f, type);

	auto stat_type = (type == nano::transport::traffic_type::vote_final) ? nano::stat::type::vote_generator_final : nano::stat::type::vote_generator;
	stats.add (stat_type, nano::stat::detail::sent_pr, sent_pr);
	stats.add (stat_type, nano::stat::detail::sent_non_pr, sent_non_pr);
}

nano::container_info nano::vote_generator::container_info () const
{
	nano::container_info info;
	info.add ("normal_verifier", normal_verifier.container_info ());
	info.add ("normal_broadcaster", normal_broadcaster.container_info ());
	info.add ("final_verifier", final_verifier.container_info ());
	info.add ("final_broadcaster", final_broadcaster.container_info ());
	return info;
}

/*
 * vote_generator_config
 */

nano::error nano::vote_generator_config::serialize (nano::tomlconfig & toml) const
{
	toml.put ("max_queue", max_queue, "Maximum number of entries in the vote generation queue. \ntype:uint64");
	toml.put ("batch_size", batch_size, "Maximum number of entries to process in a single batch. \ntype:uint64");
	toml.put ("normal_threads", normal_threads, "Number of threads for normal vote processing. \ntype:uint64");
	toml.put ("delay", delay.count (), "Delay before votes are sent to allow for efficient bundling of hashes in votes. \ntype:milliseconds");

	return toml.get_error ();
}

nano::error nano::vote_generator_config::deserialize (nano::tomlconfig & toml)
{
	toml.get ("max_queue", max_queue);
	toml.get ("batch_size", batch_size);
	toml.get ("normal_threads", normal_threads);
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
