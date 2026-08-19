#include <nano/lib/blocks.hpp>
#include <nano/lib/enum_util.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/vote.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/block_processor.hpp>
#include <nano/node/cementing_set.hpp>
#include <nano/node/election.hpp>
#include <nano/node/local_vote_history.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/online_reps.hpp>
#include <nano/node/vote_cache.hpp>
#include <nano/node/vote_generator.hpp>
#include <nano/node/vote_router.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>

using namespace std::chrono;

std::chrono::milliseconds nano::election::base_latency () const
{
	return node.network_params.network.is_dev_network () ? 25ms : 1000ms;
}

/*
 * election
 */

nano::election::election (nano::node & node_a, std::shared_ptr<nano::block> const & block_a, nano::election_behavior election_behavior_a, nano::bucket_index bucket_a, std::function<void (std::shared_ptr<nano::block> const &)> confirmation_action_a, std::function<void (nano::account const &)> vote_action_a, std::function<void (nano::qualified_root const &)> update_action_a) :
	confirmation_action (std::move (confirmation_action_a)),
	vote_action (std::move (vote_action_a)),
	update_action (std::move (update_action_a)),
	node (node_a),
	pacing ({
	.base_latency = base_latency (),
	.vote_interval = node_a.config.network_params.network.vote_broadcast_interval,
	.block_interval = node_a.config.network_params.network.block_broadcast_interval,
	}),
	ballot (block_a, [this] (nano::account const & account) { return node.ledger.weight (account); }),
	behavior_m (election_behavior_a),
	status (block_a),
	height (block_a->sideband ().height),
	root (block_a->root ()),
	qualified_root (block_a->qualified_root ()),
	account (block_a->account ()),
	bucket (bucket_a)
{
}

void nano::election::confirm_once (nano::unique_lock<nano::mutex> & lock)
{
	debug_assert (lock.owns_lock ());
	debug_assert (!mutex.try_lock ());

	bool just_confirmed = state_m != nano::election_state::confirmed;
	state_m = nano::election_state::confirmed;
	state_start = std::chrono::steady_clock::now ();

	if (just_confirmed)
	{
		status.election_end = std::chrono::system_clock::now (); // Timestamp as system time
		status.election_duration = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - election_start);
		status.confirmation_request_count = confirmation_request_count;
		status.block_count = nano::narrow_cast<decltype (status.block_count)> (ballot.block_count ());
		status.voter_count = nano::narrow_cast<decltype (status.voter_count)> (ballot.voter_count ());
		auto const status_l = status;

		node.active.recently_confirmed.put (qualified_root, status_l.winner->hash (), status_l);

		auto const extended_status = current_status_locked ();

		node.stats.inc (nano::stat::type::election, nano::stat::detail::confirm_once);
		node.logger.trace (nano::log::type::election, nano::log::detail::election_confirmed,
		nano::log::arg{ "id", id },
		nano::log::arg{ "qualified_root", qualified_root },
		nano::log::arg{ "status", extended_status });

		node.logger.debug (nano::log::type::election, "Election confirmed with winner: {} (behavior: {}, state: {}, voters: {}, blocks: {}, duration: {}ms, confirmation requests: {})",
		status_l.winner->hash (),
		to_string (behavior_m),
		to_string (state_m),
		extended_status.status.voter_count,
		extended_status.status.block_count,
		extended_status.status.election_duration.count (),
		extended_status.status.confirmation_request_count);

		node.cementing_set.add (status_l.winner->hash (), shared_from_this ());

		lock.unlock ();

		if (update_action)
		{
			node.election_workers.post ([qualified_root_l = qualified_root, update_action_l = update_action] () {
				update_action_l (qualified_root_l);
			});
		}

		if (confirmation_action)
		{
			node.election_workers.post ([status_l, confirmation_action_l = confirmation_action] () {
				confirmation_action_l (status_l.winner);
			});
		}
	}
	else
	{
		node.stats.inc (nano::stat::type::election, nano::stat::detail::confirm_once_failed);
		lock.unlock ();
	}
}

bool nano::election::valid_change (nano::election_state expected_a, nano::election_state desired_a) const
{
	switch (expected_a)
	{
		case nano::election_state::passive:
			switch (desired_a)
			{
				case nano::election_state::active:
				case nano::election_state::confirmed:
				case nano::election_state::expired_unconfirmed:
				case nano::election_state::cancelled:
					return true; // Valid
				default:
					break;
			}
			break;
		case nano::election_state::active:
			switch (desired_a)
			{
				case nano::election_state::confirmed:
				case nano::election_state::expired_unconfirmed:
				case nano::election_state::cancelled:
					return true; // Valid
				default:
					break;
			}
			break;
		case nano::election_state::confirmed:
			switch (desired_a)
			{
				case nano::election_state::expired_confirmed:
					return true; // Valid
				default:
					break;
			}
			break;
		case nano::election_state::expired_unconfirmed:
		case nano::election_state::expired_confirmed:
		case nano::election_state::cancelled:
			// No transitions are valid from these states
			break;
	}
	return false;
}

bool nano::election::state_change (nano::election_state expected_a, nano::election_state desired_a)
{
	bool result = true;
	if (valid_change (expected_a, desired_a))
	{
		if (state_m == expected_a)
		{
			state_m = desired_a;
			state_start = std::chrono::steady_clock::now ();
			result = false;

			if (update_action)
			{
				node.election_workers.post ([qualified_root_l = qualified_root, update_action_l = update_action] () {
					update_action_l (qualified_root_l);
				});
			}
		}
	}
	return result;
}

void nano::election::request_sent ()
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	pacing.request_sent (std::chrono::steady_clock::now ());
	++confirmation_request_count;

	node.stats.inc (nano::stat::type::election, nano::stat::detail::confirmation_request);
	node.logger.debug (nano::log::type::election, "Sent confirmation request for root: {} (behavior: {}, state: {}, voters: {}, blocks: {}, duration: {}ms, confirmation requests: {})",
	qualified_root,
	to_string (behavior_m),
	to_string (state_m),
	status.voter_count,
	status.block_count,
	duration ().count (),
	confirmation_request_count.load ());
}

bool nano::election::transition_priority ()
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	if (behavior_m == nano::election_behavior::priority || behavior_m == nano::election_behavior::manual)
	{
		return false;
	}

	auto const previous_behavior = behavior_m;
	behavior_m = nano::election_behavior::priority;
	pacing.reset_vote (); // Allow new outgoing votes immediately

	node.logger.debug (nano::log::type::election, "Transitioned election behavior to priority from {} for root: {} (duration: {}ms)",
	to_string (previous_behavior),
	qualified_root,
	duration ().count ());

	if (update_action)
	{
		node.election_workers.post ([qualified_root_l = qualified_root, update_action_l = update_action] () {
			update_action_l (qualified_root_l);
		});
	}

	return true;
}

bool nano::election::transition_active ()
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return !state_change (nano::election_state::passive, nano::election_state::active); // Invert since false => success
}

bool nano::election::cancel ()
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return !state_change (state_m, nano::election_state::cancelled); // Invert since false => success
}

bool nano::election::confirmed_locked () const
{
	debug_assert (!mutex.try_lock ());
	return state_m == nano::election_state::confirmed || state_m == nano::election_state::expired_confirmed;
}

bool nano::election::confirmed () const
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	return confirmed_locked ();
}

bool nano::election::failed () const
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	return state_m == nano::election_state::expired_unconfirmed;
}

void nano::election::broadcast_sent (nano::block_hash const & winner)
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	bool const initial = pacing.is_first_block ();
	pacing.block_sent (winner, std::chrono::steady_clock::now ());

	node.stats.inc (nano::stat::type::election, initial ? nano::stat::detail::broadcast_block_initial : nano::stat::detail::broadcast_block_repeat);

	node.logger.debug (nano::log::type::election, "Broadcasted current winner: {} for root: {} (behavior: {}, state: {}, voters: {}, blocks: {}, duration: {}ms)",
	winner,
	qualified_root,
	to_string (behavior_m),
	to_string (state_m),
	status.voter_count,
	status.block_count,
	duration ().count ());
}

nano::vote_info nano::election::get_last_vote (nano::account const & account)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return ballot.get_vote (account);
}

void nano::election::set_last_vote (nano::account const & account, nano::vote_info vote_info)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	ballot.set_vote (account, vote_info);
}

nano::election_status nano::election::get_status () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return status;
}

nano::election_actions nano::election::tick (std::chrono::steady_clock::time_point now)
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	nano::election_actions actions;
	switch (state_m)
	{
		case nano::election_state::passive:
			if (base_latency () * passive_duration_factor < now - state_start)
			{
				state_change (nano::election_state::passive, nano::election_state::active);
			}
			break;
		case nano::election_state::active:
			broadcast_vote_locked (now);
			actions.broadcast = pacing.due_block (status.winner->hash (), now);
			actions.request = pacing.due_request (behavior_m, now);
			if (actions.broadcast || actions.request)
			{
				actions.snapshot = snapshot_locked ();
			}
			break;
		case nano::election_state::confirmed:
			actions.cleanup = true; // Election is done and should be cleaned up
			if (pacing.due_block (status.winner->hash (), now))
			{
				actions.broadcast = true; // Ensure election winner is broadcasted
				actions.snapshot = snapshot_locked ();
			}
			state_change (nano::election_state::confirmed, nano::election_state::expired_confirmed);
			break;
		case nano::election_state::expired_unconfirmed:
		case nano::election_state::expired_confirmed:
			debug_assert (false);
			break;
		case nano::election_state::cancelled:
			actions.cleanup = true; // Clean up cancelled elections immediately
			return actions;
	}

	if (!confirmed_locked () && time_to_live () < now - election_start)
	{
		// It is possible the election confirmed while acquiring the mutex
		// state_change returning true would indicate it
		if (!state_change (state_m, nano::election_state::expired_unconfirmed))
		{
			node.logger.trace (nano::log::type::election, nano::log::detail::election_expired,
			nano::log::arg{ "id", id },
			nano::log::arg{ "qualified_root", qualified_root },
			nano::log::arg{ "status", current_status_locked () });

			actions.cleanup = true; // Election expired and should be cleaned up
			status.type = nano::election_status_type::stopped;
		}
	}

	return actions;
}

std::chrono::milliseconds nano::election::time_to_live () const
{
	switch (behavior_m)
	{
		case election_behavior::manual:
		case election_behavior::priority:
			return std::chrono::milliseconds (5 * 60 * 1000);
		case election_behavior::hinted:
		case election_behavior::optimistic:
			return std::chrono::milliseconds (30 * 1000);
	}
	debug_assert (false);
	return {};
}

nano::uint128_t nano::election::quorum_delta () const
{
	return node.online_reps.delta ();
}

nano::tally_t nano::election::tally () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return ballot.tally ();
}

void nano::election::confirm_if_quorum (nano::unique_lock<nano::mutex> & lock_a)
{
	debug_assert (lock_a.owns_lock ());

	auto const delta = quorum_delta ();
	auto const result = ballot.evaluate (delta);
	if (result.winner == nullptr)
	{
		return; // No vote for a known block has been recorded yet
	}

	status.tally = result.winner_weight;
	status.final_tally = result.final_weight;

	auto const & winner_hash = result.winner->hash ();
	auto const & status_winner_hash = status.winner->hash ();
	// Only switch the winning fork once the total participating weight reaches the quorum delta
	if (result.total_weight >= delta && winner_hash != status_winner_hash)
	{
		status.winner = result.winner;
		remove_votes (status_winner_hash);

		node.logger.debug (nano::log::type::election, "Winning fork changed from {} to {} for root: {} (behavior: {}, state: {}, voters: {}, blocks: {}, duration: {}ms)",
		status_winner_hash,
		winner_hash,
		qualified_root,
		to_string (behavior_m),
		to_string (state_m),
		status.voter_count,
		status.block_count,
		duration ().count ());

		node.block_processor.force (result.winner);
	}
	if (result.quorum)
	{
		if (!is_quorum.exchange (true) && node.is_voting ())
		{
			++vote_broadcast_count;
			node.vote_generator.vote_final (qualified_root, status.winner->hash (), bucket);
		}
		if (result.final_quorum)
		{
			// In some edge cases block might get rolled back while the election is confirming, reprocess it to ensure it's present in the ledger
			node.block_processor.add (result.winner, nano::block_source::election);
			confirm_once (lock_a);
			debug_assert (!lock_a.owns_lock ());
		}
	}
}

void nano::election::try_confirm (nano::block_hash const & hash)
{
	nano::unique_lock<nano::mutex> election_lock{ mutex };
	auto winner = status.winner;
	if (winner && winner->hash () == hash)
	{
		if (!confirmed_locked ())
		{
			confirm_once (election_lock);
			debug_assert (!election_lock.owns_lock ());
		}
	}
}

std::shared_ptr<nano::block> nano::election::find (nano::block_hash const & hash_a) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return ballot.find (hash_a);
}

nano::vote_code nano::election::vote (nano::account const & rep, uint64_t timestamp_a, nano::block_hash const & block_hash_a, nano::vote_source vote_source_a)
{
	auto const weight = node.ledger.weight (rep);

	if (!node.network_params.network.is_dev_network () && weight <= node.minimum_principal_weight ())
	{
		return vote_code::indeterminate;
	}

	// Only cooldown live votes
	auto const cooldown = vote_source_a != vote_source::cache ? nano::vote_cooldown (weight, node.online_reps.trended ()) : 0s;

	nano::unique_lock<nano::mutex> lock{ mutex };

	switch (ballot.insert_vote (rep, timestamp_a, block_hash_a, std::chrono::steady_clock::now (), cooldown))
	{
		case nano::election_ballot::vote_result::replay:
			return vote_code::replay;
		case nano::election_ballot::vote_result::ignored:
			return vote_code::ignored;
		case nano::election_ballot::vote_result::accepted:
			break;
	}

	node.stats.inc (nano::stat::type::election, nano::stat::detail::vote);
	node.stats.inc (nano::stat::type::election_vote, to_stat_detail (vote_source_a));

	node.logger.trace (nano::log::type::election, nano::log::detail::vote_processed,
	nano::log::arg{ "id", id },
	nano::log::arg{ "qualified_root", qualified_root },
	nano::log::arg{ "account", rep },
	nano::log::arg{ "hash", block_hash_a },
	nano::log::arg{ "final", nano::vote::is_final_timestamp (timestamp_a) },
	nano::log::arg{ "timestamp", timestamp_a },
	nano::log::arg{ "vote_source", vote_source_a },
	nano::log::arg{ "weight", weight });

	node.logger.debug (nano::log::type::election, "Vote received for hash: {} from: {} for root: {} (final: {}, weight: {}, source: {})",
	block_hash_a,
	rep,
	qualified_root,
	nano::vote::is_final_timestamp (timestamp_a),
	weight,
	to_string (vote_source_a));

	// This must execute before calculating the vote tally to ensure accurate online weight and quorum numbers are used
	if (vote_action)
	{
		vote_action (rep);
	}

	if (!confirmed_locked ())
	{
		confirm_if_quorum (lock);
	}

	return vote_code::vote;
}

bool nano::election::publish (std::shared_ptr<nano::block> const & block_a)
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	// Do not insert new blocks if already confirmed
	auto result (confirmed_locked ());
	if (!result && ballot.full () && !ballot.contains (block_a->hash ()))
	{
		if (!replace_by_weight (lock, block_a->hash ()))
		{
			result = true; // The new block is too weak to evict any existing one
			node.network.filter.clear (block_a);
		}
		debug_assert (lock.owns_lock ());
	}
	if (!result)
	{
		if (!ballot.insert_block (block_a))
		{
			result = true; // Block was already present, its contents were replaced
			if (status.winner->hash () == block_a->hash ())
			{
				status.winner = block_a;
			}
		}
	}
	return result;
}

nano::election_snapshot nano::election::snapshot_locked () const
{
	debug_assert (!mutex.try_lock ());
	return { qualified_root, status.winner, is_quorum.load (), ballot.votes () };
}

nano::election_extended_status nano::election::current_status () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return current_status_locked ();
}

nano::election_extended_status nano::election::current_status_locked () const
{
	debug_assert (!mutex.try_lock ());

	nano::election_status status_l = status;
	status_l.confirmation_request_count = confirmation_request_count;
	status_l.vote_broadcast_count = vote_broadcast_count;
	status_l.block_count = nano::narrow_cast<decltype (status_l.block_count)> (ballot.block_count ());
	status_l.voter_count = nano::narrow_cast<decltype (status_l.voter_count)> (ballot.voter_count ());
	return nano::election_extended_status{ status_l, ballot.votes (), ballot.blocks (), ballot.tally () };
}

std::shared_ptr<nano::block> nano::election::winner () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return status.winner;
}

std::chrono::milliseconds nano::election::duration () const
{
	return std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - election_start);
}

void nano::election::broadcast_vote ()
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	broadcast_vote_locked (std::chrono::steady_clock::now ());
}

void nano::election::broadcast_vote_locked (std::chrono::steady_clock::time_point now)
{
	debug_assert (!mutex.try_lock ());

	if (!node.is_voting ())
	{
		return;
	}
	if (!pacing.due_vote (now))
	{
		return;
	}
	pacing.vote_sent (now);

	// Broadcast a final vote if reached quorum or already confirmed
	bool const is_final = confirmed_locked () || ballot.evaluate (quorum_delta ()).quorum;

	node.stats.inc (nano::stat::type::election, nano::stat::detail::broadcast_vote);
	node.stats.inc (nano::stat::type::election, is_final ? nano::stat::detail::broadcast_vote_final : nano::stat::detail::broadcast_vote_normal);
	++vote_broadcast_count;

	node.logger.trace (nano::log::type::election, nano::log::detail::broadcast_vote,
	nano::log::arg{ "id", id },
	nano::log::arg{ "qualified_root", qualified_root },
	nano::log::arg{ "winner", status.winner },
	nano::log::arg{ "type", is_final ? "final" : "normal" });

	node.vote_generator.vote (qualified_root, status.winner->hash (), bucket, is_final ? nano::vote_type::final : nano::vote_type::normal);
}

void nano::election::remove_votes (nano::block_hash const & hash_a)
{
	debug_assert (!mutex.try_lock ());
	if (node.is_voting ())
	{
		// Remove votes from election
		auto generated_votes = node.history.votes (root, hash_a);
		std::vector<nano::account> reps;
		reps.reserve (generated_votes.size ());
		std::transform (generated_votes.begin (), generated_votes.end (), std::back_inserter (reps), [] (auto const & vote) { return vote->account; });
		ballot.erase_votes (reps);
		// Clear votes cache
		node.history.erase (root);
	}
}

bool nano::election::replace_by_weight (nano::unique_lock<nano::mutex> & lock_a, nano::block_hash const & hash_a)
{
	debug_assert (lock_a.owns_lock ());
	auto const winner_hash = status.winner->hash ();
	lock_a.unlock ();

	// Weight backing the new block in the vote cache, it replaces an existing block only if it outweighs it
	nano::uint128_t inactive_tally{ 0 };
	for (auto const & vote : node.vote_cache.find (hash_a))
	{
		inactive_tally += node.ledger.weight (vote->account);
	}

	lock_a.lock ();
	auto const replaced_block = ballot.replacement_candidate (inactive_tally, winner_hash);
	if (!replaced_block)
	{
		return false;
	}

	// Disconnect outside the election mutex to avoid lock inversion with the vote router
	lock_a.unlock ();
	node.vote_router.disconnect (*replaced_block);
	lock_a.lock ();

	if (auto removed = ballot.erase_block (*replaced_block, status.winner->hash ()))
	{
		node.network.filter.clear (removed);
	}
	return true;
}

void nano::election::force_confirm ()
{
	release_assert (node.network_params.network.is_dev_network ());
	nano::unique_lock<nano::mutex> lock{ mutex };
	confirm_once (lock);
}

std::unordered_set<nano::block_hash> nano::election::blocks_hashes () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return ballot.blocks_hashes ();
}

std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> nano::election::blocks () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return ballot.blocks ();
}

std::unordered_map<nano::account, nano::vote_info> nano::election::votes () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return ballot.votes ();
}

std::vector<nano::vote_with_weight_info> nano::election::votes_with_weight () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return ballot.votes_with_weight ();
}

nano::election_behavior nano::election::behavior () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return behavior_m;
}

nano::election_state nano::election::state () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return state_m;
}

bool nano::election::contains (nano::block_hash const & hash) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return ballot.contains (hash);
}

size_t nano::election::voter_count () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return ballot.voter_count ();
}

size_t nano::election::block_count () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return ballot.block_count ();
}

void nano::election::operator() (nano::object_stream & obs) const
{
	obs.write ("id", id);
	obs.write ("qualified_root", qualified_root);
	obs.write ("behavior", behavior_m);
	obs.write ("height", height);
	obs.write ("status", current_status ());
}

void nano::election_extended_status::operator() (nano::object_stream & obs) const
{
	obs.write ("winner", status.winner->hash ());
	obs.write ("tally_amount", status.tally.to_string_dec ());
	obs.write ("final_tally_amount", status.final_tally.to_string_dec ());
	obs.write ("confirmation_request_count", status.confirmation_request_count);
	obs.write ("vote_broadcast_count", status.vote_broadcast_count);
	obs.write ("block_count", status.block_count);
	obs.write ("voter_count", status.voter_count);
	obs.write ("type", status.type);

	obs.write_range ("votes", votes, [] (auto const & entry, nano::object_stream & obs) {
		auto & [account, info] = entry;
		obs.write ("account", account);
		obs.write ("hash", info.hash);
		obs.write ("final", nano::vote::is_final_timestamp (info.timestamp));
		obs.write ("timestamp", info.timestamp);
		obs.write ("time", info.time.time_since_epoch ().count ());
	});

	obs.write_range ("blocks", blocks, [] (auto const & entry) {
		auto [hash, block] = entry;
		return block;
	});

	obs.write_range ("tally", tally, [] (auto const & entry, nano::object_stream & obs) {
		auto & [amount, block] = entry;
		obs.write ("hash", block->hash ());
		obs.write ("amount", amount);
	});
}

/*
 *
 */

std::string_view nano::to_string (nano::election_behavior behavior)
{
	return nano::enum_to_string (behavior);
}

nano::stat::detail nano::to_stat_detail (nano::election_behavior behavior)
{
	return nano::enum_convert<nano::stat::detail> (behavior);
}

std::string_view nano::to_string (nano::election_state state)
{
	return nano::enum_to_string (state);
}

nano::stat::detail nano::to_stat_detail (nano::election_state state)
{
	return nano::enum_convert<nano::stat::detail> (state);
}
