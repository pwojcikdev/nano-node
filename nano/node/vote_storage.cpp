#include <nano/lib/blocks.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/vote_storage.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>

nano::vote_storage::vote_storage (vote_storage_config const & config_a, nano::node & node_a, nano::store::component & vote_store_a, nano::network & network_a, nano::ledger & ledger_a, nano::stats & stats_a) :
	config{ config_a },
	node{ node_a },
	vote_store{ vote_store_a },
	network{ network_a },
	ledger{ ledger_a },
	stats{ stats_a },
	store_queue{ stats, nano::stat::type::vote_storage_write, nano::thread_role::name::vote_storage, /* single threaded */ 1, config_a.max_store_queue, 1024 },
	reply_queue{ stats, nano::stat::type::vote_storage_replies, nano::thread_role::name::vote_storage, /* threads */ 1, config_a.max_reply_queue, 512 }
{
	store_queue.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};

	reply_queue.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};
}

nano::vote_storage::~vote_storage ()
{
	// All threads should be stopped before destruction
	debug_assert (!store_queue.joinable ());
	debug_assert (!reply_queue.joinable ());
	debug_assert (!thread.joinable ());
}

void nano::vote_storage::start ()
{
	store_queue.start ();
	reply_queue.start ();

	debug_assert (!thread.joinable ());

	if (!config.enable || !config.enable_broadcast)
	{
		return;
	}

	thread = std::thread ([this] {
		nano::thread_role::set (nano::thread_role::name::vote_storage);
		run ();
	});
}

void nano::vote_storage::stop ()
{
	store_queue.stop ();
	reply_queue.stop ();

	{
		nano::unique_lock<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

void nano::vote_storage::vote (std::shared_ptr<nano::vote> vote)
{
	if (!config.enable)
	{
		return;
	}
	if (config.ignore_255_votes && vote->hashes.size () > 12)
	{
		return;
	}
	if (config.store_final_only && !vote->is_final ())
	{
		return;
	}
	if (ledger.weight (vote->account) < config.rep_weight_threshold)
	{
		return;
	}
	store_queue.add (vote);
}

void nano::vote_storage::trigger (const nano::block_hash & hash, const std::shared_ptr<nano::transport::channel> & channel)
{
	if (!config.enable_broadcast && !config.enable_replies)
	{
		return;
	}

	bool const is_pr = node.rep_crawler.is_pr (channel);

	if (config.enable_broadcast)
	{
		if (!config.trigger_pr_only || is_pr)
		{
			std::lock_guard guard{ mutex };

			auto [it, _] = requests.emplace (request_entry{ hash, 0 });
			requests.modify (it, [] (auto & entry) {
				++entry.count;
			});

			while (requests.size () > max_requests)
			{
				requests.get<tag_sequenced> ().pop_front ();
			}
		}
	}

	if (config.enable_replies)
	{
		if (!channel->max (nano::transport::traffic_type::vote_storage))
		{
			if (!recently_broadcasted.check (hash, channel))
			{
				reply_queue.add (reply_entry_t{ hash, channel });
			}
		}
	}
}

void nano::vote_storage::run ()
{
	std::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		condition.wait_for (lock, std::chrono::seconds{ 1 }, [this] {
			return stopped || !requests.empty ();
		});

		if (stopped)
		{
			return;
		}

		stats.inc (nano::stat::type::vote_storage, nano::stat::detail::loop);

		cleanup ();

		if (!requests.empty ())
		{
			auto requests_l = requests; // Copy

			lock.unlock ();

			auto broadcasted = run_broadcasts (requests_l);

			lock.lock ();

			for (auto & hash : broadcasted)
			{
				requests.erase (hash);
			}
		}
	}
}

void nano::vote_storage::cleanup ()
{
	debug_assert (!mutex.try_lock ());

	erase_if (requests, [this] (auto const & entry) {
		return nano::elapsed (entry.time, config.request_age_cutoff);
	});

	erase_if (requests, [this] (auto const & entry) {
		return recently_broadcasted.check (entry.hash);
	});
}

std::unordered_set<nano::block_hash> nano::vote_storage::run_broadcasts (ordered_requests requests_l)
{
	debug_assert (!requests_l.empty ());

	size_t constexpr max_broadcasts = 16;

	auto vote_transaction = vote_store.tx_begin_read ();

	std::unordered_set<nano::block_hash> broadcasted;

	for (auto & entry : requests_l.get<tag_count> ())
	{
		//		std::cout << entry.count << std::endl;

		auto votes = query_hash (vote_transaction, entry.hash);
		if (!votes.empty ())
		{
			bool recent = recently_broadcasted.check_and_insert (entry.hash);
			debug_assert (!recent); // Should be filtered out earlier

			stats.inc (nano::stat::type::vote_storage, nano::stat::detail::process);

			wait_peers ();
			broadcast (votes, entry.hash);

			broadcasted.insert (entry.hash);
		}
		else
		{
			stats.inc (nano::stat::type::vote_storage, nano::stat::detail::empty);
		}

		if (broadcasted.size () >= max_broadcasts)
		{
			break;
		}
	}

	return broadcasted;
}

void nano::vote_storage::wait_peers ()
{
	auto reps = node.rep_crawler.principal_representatives ();

	auto busy_count = [&reps] () {
		return std::count_if (reps.begin (), reps.end (), [] (auto const & rep) {
			return rep.channel->max (nano::transport::traffic_type::vote_storage);
		});
	};

	while (busy_count () > reps.size () * config.max_busy_ratio)
	{
		if (stopped)
		{
			return;
		}

		std::this_thread::sleep_for (std::chrono::milliseconds{ 100 });
	}
}

void nano::vote_storage::process_batch (decltype (store_queue)::batch_t & batch)
{
	auto vote_transaction = vote_store.tx_begin_write ();

	for (auto & vote : batch)
	{
		auto result = vote_store.vote_storage.put (vote_transaction, vote);
		if (result > 0)
		{
			stats.inc (nano::stat::type::vote_storage_write, nano::stat::detail::stored);
			stats.add (nano::stat::type::vote_storage_write, nano::stat::detail::stored_votes, nano::stat::dir::in, result);
		}
	}
}

void nano::vote_storage::process_batch (decltype (reply_queue)::batch_t & batch)
{
	auto vote_transaction = vote_store.tx_begin_read ();
	//	auto ledger_transaction = ledger.store.tx_begin_read ();

	for (auto & [hash, channel] : batch)
	{
		// Check votes for specific hash
		{
			auto votes = query_hash (vote_transaction, hash, /* final only */ true);
			if (!votes.empty ())
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::reply);

				reply (votes, hash, channel);

				//				if (config.enable_broadcast)
				//				{
				//					broadcast (votes, hash);
				//				}
			}
			else
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::empty);
			}
		}

		// Check votes for frontier
		//		if (config.enable_query_frontier)
		//		{
		//			auto [frontier_votes, frontier_hash] = query_frontier (ledger_transaction, vote_transaction, hash);
		//			if (!frontier_votes.empty ())
		//			{
		//				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::frontier);
		//
		//				reply (frontier_votes, channel);
		//
		//				if (config.enable_broadcast)
		//				{
		//					broadcast (frontier_votes, frontier_hash);
		//				}
		//			}
		//			else
		//			{
		//				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::frontier_empty);
		//			}
		//		}
	}
}

void nano::vote_storage::reply (const nano::vote_storage::vote_list_t & votes, const nano::block_hash & hash, const std::shared_ptr<nano::transport::channel> & channel)
{
	//	if (channel->max (nano::transport::traffic_type::vote_storage)) // TODO: Scrutinize this
	//	{
	//		stats.inc (nano::stat::type::vote_storage, nano::stat::detail::reply_channel_full, nano::stat::dir::out);
	//		return;
	//	}

	if (recently_broadcasted.check_and_insert (hash, channel))
	{
		stats.inc (nano::stat::type::vote_storage, nano::stat::detail::reply_duplicate, nano::stat::dir::out);
		return;
	}

	stats.inc (nano::stat::type::vote_storage, nano::stat::detail::reply, nano::stat::dir::out);
	stats.add (nano::stat::type::vote_storage, nano::stat::detail::reply_vote, nano::stat::dir::out, votes.size ());

	for (auto & vote : votes)
	{
		nano::confirm_ack message{ node.network_params.network, vote, true };

		channel->send (
		message, nano::transport::traffic_type::vote_storage, [this] (auto & ec, auto size) {
			if (ec)
			{
				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::write_error_reply, nano::stat::dir::out);
			}
		});
	}
}

void nano::vote_storage::broadcast (const nano::vote_storage::vote_list_t & votes, const nano::block_hash & hash)
{
	stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast, nano::stat::dir::out);

	if (config.enable_pr_broadcast)
	{
		auto pr_nodes = node.rep_crawler.principal_representatives ();
		for (auto const & rep : pr_nodes)
		{
			//			if (rep.channel->max (nano::transport::traffic_type::vote_storage)) // TODO: Scrutinize this
			//			{
			//				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_channel_full, nano::stat::dir::out);
			//				continue;
			//			}

			//			if (recently_broadcasted (rep.channel, hash))
			//			{
			//				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_duplicate, nano::stat::dir::out);
			//				continue;
			//			}

			stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_rep, nano::stat::dir::out);
			stats.add (nano::stat::type::vote_storage, nano::stat::detail::broadcast_vote, nano::stat::dir::out, votes.size ());

			for (auto & vote : votes)
			{
				nano::confirm_ack message{ node.network_params.network, vote };

				rep.channel->send (
				message, nano::transport::traffic_type::vote_storage, [this] (auto & ec, auto size) {
					if (ec)
					{
						stats.inc (nano::stat::type::vote_storage, nano::stat::detail::write_error_broadcast, nano::stat::dir::out);
					}
				});
			}
		}
	}

	//	if (config.enable_random_broadcast)
	//	{
	//		auto random_nodes = network.list (network.fanout ());
	//		for (auto const & channel : random_nodes)
	//		{
	//			if (channel->max (nano::transport::traffic_type::vote_storage)) // TODO: Scrutinize this
	//			{
	//				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_channel_full, nano::stat::dir::in);
	//				continue;
	//			}
	//
	//			if (recently_broadcasted (channel, hash))
	//			{
	//				stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_duplicate, nano::stat::dir::in);
	//				continue;
	//			}
	//
	//			stats.inc (nano::stat::type::vote_storage, nano::stat::detail::broadcast_random, nano::stat::dir::in);
	//			stats.add (nano::stat::type::vote_storage, nano::stat::detail::broadcast_vote, nano::stat::dir::in, votes.size ());
	//
	//			for (auto & vote : votes)
	//			{
	//				nano::confirm_ack message{ node.network_params.network, vote };
	//
	//				channel->send (
	//				message, [this] (auto & ec, auto size) {
	//					if (ec)
	//					{
	//						stats.inc (nano::stat::type::vote_storage, nano::stat::detail::write_error_broadcast, nano::stat::dir::in);
	//					}
	//				},
	//				nano::transport::buffer_drop_policy::no_socket_drop, nano::transport::traffic_type::vote_storage);
	//			}
	//		}
	//	}
}

nano::uint128_t nano::vote_storage::weight (const nano::vote_storage::vote_list_t & votes) const
{
	nano::uint128_t result = 0;
	for (auto const & vote : votes)
	{
		result += ledger.weight (vote->account);
	}
	return result;
}

nano::uint128_t nano::vote_storage::weight_final (const nano::vote_storage::vote_list_t & votes) const
{
	nano::uint128_t result = 0;
	for (auto const & vote : votes)
	{
		if (vote->is_final ())
		{
			result += ledger.weight (vote->account);
		}
	}
	return result;
}

nano::vote_storage::vote_list_t nano::vote_storage::filter (const nano::vote_storage::vote_list_t & votes) const
{
	nano::vote_storage::vote_list_t result;
	for (auto const & vote : votes)
	{
		auto should_pass = [this] (auto const & vote) {
			if (config.ignore_255_votes && vote->hashes.size () > 12)
			{
				return false;
			}
			return ledger.weight (vote->account) >= config.rep_weight_threshold;
		};

		if (should_pass (vote))
		{
			result.push_back (vote);
		}
	}
	return result;
}

nano::vote_storage::vote_list_t nano::vote_storage::query_hash (const nano::store::transaction & vote_transaction, const nano::block_hash & hash, bool final_only)
{
	auto votes = vote_store.vote_storage.get (vote_transaction, hash);
	if (!votes.empty ())
	{
		auto const quorum = node.online_reps.delta ();

		//		auto should_pass = [this] (auto const & votes) {
		//			if (config.vote_final_weight_threshold > 0 && weight_final (votes) >= config.vote_final_weight_threshold)
		//			{
		//				return true;
		//			}
		//			if (config.vote_weight_threshold > 0 && weight (votes) >= config.vote_weight_threshold)
		//			{
		//				return true;
		//			}
		//			return false;
		//		};

		//		if (should_pass (votes))

		auto const wgt = final_only ? weight_final (votes) : weight (votes);
		if (wgt >= quorum)
		{
			return filter (votes);
		}
		else
		{
			stats.inc (nano::stat::type::vote_storage, nano::stat::detail::low_weight);
		}
	}
	return {};
}

nano::vote_storage::vote_list_t nano::vote_storage::query_hash (nano::block_hash hash)
{
	auto vote_transaction = vote_store.tx_begin_read ();
	return query_hash (vote_transaction, hash);
}

std::pair<nano::vote_storage::vote_list_t, nano::block_hash> nano::vote_storage::query_frontier (nano::secure::transaction const & ledger_transaction, nano::store::transaction const & vote_transaction, const nano::block_hash & hash)
{
	auto block = ledger.any.block_get (ledger_transaction, hash);
	if (!block)
	{
		return {};
	}
	auto account = block->account ();

	auto maybe_account_info = ledger.any.account_get (ledger_transaction, account);
	if (!maybe_account_info)
	{
		return {};
	}
	auto account_info = maybe_account_info.value ();

	auto frontier = account_info.head;

	const int max_retries = 128;
	for (int n = 0; n < max_retries && !frontier.is_zero () && frontier != hash; ++n)
	{
		auto votes = query_hash (vote_transaction, frontier);
		if (!votes.empty ())
		{
			return { votes, frontier };
		}

		// TODO: Create `ledger.previous(hash)` helper
		auto block = ledger.store.block.get (ledger_transaction, frontier);
		if (block)
		{
			frontier = block->previous ();
		}
		else
		{
			frontier = { 0 };
		}
	}

	return {};
}

nano::container_info nano::vote_storage::container_info () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	nano::container_info info;
	info.put ("requests", requests.size (), sizeof (decltype (requests)::value_type));
	info.put ("store_queue", store_queue.size ());
	info.put ("broadcast_queue", reply_queue.size ());
	info.put ("recently_broadcasted", recently_broadcasted.total_size ());
	return info;
}

/*
 * recently_broadcasted
 */

bool nano::vote_storage::recently_broadcasted::check (const nano::block_hash & hash)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	cleanup ();
	return recently_broadcasted_hashes.contains (hash);
}

bool nano::vote_storage::recently_broadcasted::check_and_insert (const nano::block_hash & hash)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	cleanup ();

	if (recently_broadcasted_hashes.contains (hash))
	{
		return true;
	}
	recently_broadcasted_hashes.emplace (hash, std::chrono::steady_clock::now ());
	return false;
}

bool nano::vote_storage::recently_broadcasted::check (const nano::block_hash & hash, const std::shared_ptr<nano::transport::channel> & channel)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	cleanup ();

	if (auto it = recently_broadcasted_map.find (channel); it != recently_broadcasted_map.end ())
	{
		auto & entries = it->second;
		return entries.contains (hash);
	}
	return false;
}

bool nano::vote_storage::recently_broadcasted::check_and_insert (const nano::block_hash & hash, const std::shared_ptr<nano::transport::channel> & channel)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	cleanup ();

	if (auto it = recently_broadcasted_map.find (channel); it != recently_broadcasted_map.end ())
	{
		auto & entries = it->second;
		if (entries.contains (hash))
		{
			return true;
		}
	}
	recently_broadcasted_map[channel].emplace (hash, std::chrono::steady_clock::now ());
	return false;
}

void nano::vote_storage::recently_broadcasted::cleanup ()
{
	debug_assert (!mutex.try_lock ());

	// Cleanup
	if (elapsed (last_cleanup, cleanup_interval))
	{
		last_cleanup = std::chrono::steady_clock::now ();

		for (auto & [channel, entries] : recently_broadcasted_map)
		{
			erase_if (entries, [&] (auto const & entry) {
				auto const & [hash, time] = entry;
				return nano::elapsed (time, rebroadcast_interval);
			});
		}

		erase_if (recently_broadcasted_map, [&] (auto const & pair) {
			auto const & [channel, entries] = pair;
			if (!channel->alive ())
			{
				return true; // Erase
			}
			if (entries.empty ())
			{
				return true; // Erase
			}
			return false;
		});

		erase_if (recently_broadcasted_hashes, [&] (auto const & pair) {
			auto const & [hash, time] = pair;
			return nano::elapsed (time, rebroadcast_interval);
		});
	}
}

size_t nano::vote_storage::recently_broadcasted::total_size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return std::accumulate (recently_broadcasted_map.begin (), recently_broadcasted_map.end (), std::size_t{ 0 }, [] (auto total, auto const & entry) {
		return total + entry.second.size ();
	});
}