#include <nano/lib/assert.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/node/election_ballot.hpp>

#include <algorithm>
#include <iterator>
#include <limits>

std::chrono::seconds nano::vote_cooldown (nano::uint128_t weight, nano::uint128_t online_stake)
{
	if (weight > online_stake / 20) // Reps with more than 5% weight
	{
		return std::chrono::seconds{ 1 };
	}
	if (weight > online_stake / 100) // Reps with more than 1% weight
	{
		return std::chrono::seconds{ 5 };
	}
	// The rest of smaller reps
	return std::chrono::seconds{ 15 };
}

nano::election_ballot::election_ballot (std::shared_ptr<nano::block> const & initial, weight_fn weight_a, std::chrono::steady_clock::time_point now) :
	weight{ std::move (weight_a) }
{
	debug_assert (initial != nullptr);
	last_blocks.emplace (initial->hash (), initial);
	last_votes.emplace (nano::account::null (), nano::vote_info{ now, 0, initial->hash () });
}

/*
 * Votes
 */

nano::election_ballot::vote_result nano::election_ballot::insert_vote (nano::account const & rep, uint64_t timestamp, nano::block_hash const & hash, std::chrono::steady_clock::time_point now, std::chrono::seconds cooldown)
{
	if (auto existing = last_votes.find (rep); existing != last_votes.end ())
	{
		auto const last_vote = existing->second;
		if (last_vote.timestamp > timestamp)
		{
			return vote_result::replay;
		}
		// Break ties between votes with equal timestamps by hash
		if (last_vote.timestamp == timestamp && !(last_vote.hash < hash))
		{
			return vote_result::replay;
		}
		// Final votes are never throttled
		bool const final_vote = timestamp == std::numeric_limits<uint64_t>::max () && last_vote.timestamp < timestamp;
		bool const past_cooldown = last_vote.time <= now - cooldown;
		if (!final_vote && !past_cooldown)
		{
			return vote_result::ignored;
		}
	}

	last_votes[rep] = { now, timestamp, hash };
	return vote_result::accepted;
}

nano::vote_info nano::election_ballot::get_vote (nano::account const & rep) const
{
	if (auto existing = last_votes.find (rep); existing != last_votes.end ())
	{
		return existing->second;
	}
	return {};
}

void nano::election_ballot::set_vote (nano::account const & rep, nano::vote_info vote)
{
	last_votes[rep] = vote;
}

void nano::election_ballot::erase_votes (std::vector<nano::account> const & reps)
{
	for (auto const & rep : reps)
	{
		last_votes.erase (rep);
	}
}

/*
 * Blocks
 */

bool nano::election_ballot::insert_block (std::shared_ptr<nano::block> const & block)
{
	debug_assert (block != nullptr);
	auto existing = last_blocks.find (block->hash ());
	if (existing == last_blocks.end ())
	{
		last_blocks.emplace (block->hash (), block);
		return true;
	}
	existing->second = block; // Replace block contents (e.g. with a higher work value)
	return false;
}

std::optional<nano::block_hash> nano::election_ballot::replacement_candidate (nano::uint128_t inactive_tally, nano::block_hash const & winner) const
{
	if (inactive_tally == 0)
	{
		return std::nullopt;
	}

	// Weakest current block other than the winner, weighing blocks by the last evaluated tally; ties broken by lower hash
	std::optional<std::pair<nano::uint128_t, nano::block_hash>> weakest;
	for (auto const & [hash, block] : last_blocks)
	{
		if (hash == winner)
		{
			continue;
		}
		auto const tallied = last_tally.find (hash);
		nano::uint128_t const tally = tallied != last_tally.end () ? tallied->second : 0;
		std::pair<nano::uint128_t, nano::block_hash> const entry{ tally, hash };
		if (!weakest || entry < *weakest)
		{
			weakest = entry;
		}
	}

	// Evict the weakest block only if the new block outweighs it
	if (weakest && inactive_tally > weakest->first)
	{
		return weakest->second;
	}
	return std::nullopt;
}

std::shared_ptr<nano::block> nano::election_ballot::erase_block (nano::block_hash const & hash, nano::block_hash const & winner)
{
	if (hash == winner)
	{
		return nullptr;
	}
	auto existing = last_blocks.find (hash);
	if (existing == last_blocks.end ())
	{
		return nullptr;
	}
	auto block = existing->second;
	erase_if (last_votes, [&hash] (auto const & entry) {
		return entry.second.hash == hash;
	});
	last_blocks.erase (existing);
	return block;
}

bool nano::election_ballot::full () const
{
	return last_blocks.size () >= max_blocks;
}

/*
 * Tally
 */

auto nano::election_ballot::compute_weights () const -> weights
{
	weights result;
	for (auto const & [account, info] : last_votes)
	{
		auto const rep_weight = weight (account);
		result.block_weights[info.hash] += rep_weight;
		if (info.timestamp == std::numeric_limits<uint64_t>::max ())
		{
			result.final_weights[info.hash] += rep_weight;
		}
	}
	return result;
}

nano::tally_t nano::election_ballot::make_tally (std::unordered_map<nano::block_hash, nano::uint128_t> const & block_weights) const
{
	nano::tally_t result;
	for (auto const & [hash, amount] : block_weights)
	{
		if (auto block = last_blocks.find (hash); block != last_blocks.end ())
		{
			result.emplace (amount, block->second);
		}
	}
	return result;
}

nano::tally_t nano::election_ballot::tally () const
{
	return make_tally (compute_weights ().block_weights);
}

auto nano::election_ballot::evaluate (nano::uint128_t delta) -> tally_result
{
	auto const [block_weights, final_weights] = compute_weights ();
	last_tally = block_weights;

	tally_result result;
	result.tally = make_tally (block_weights);
	if (result.tally.empty ())
	{
		return result;
	}

	auto const & top = *result.tally.begin ();
	result.winner = top.second;
	result.winner_weight = top.first;

	if (auto final_it = final_weights.find (result.winner->hash ()); final_it != final_weights.end ())
	{
		result.final_weight = final_it->second;
	}

	for (auto const & [amount, block] : result.tally)
	{
		result.total_weight += amount;
	}

	// Quorum is reached once the winner leads the runner-up by at least the online weight delta
	auto runner_up = std::next (result.tally.begin ());
	auto const second = runner_up != result.tally.end () ? runner_up->first : 0;
	release_assert (result.winner_weight >= second);
	result.quorum = (result.winner_weight - second) >= delta;
	result.final_quorum = result.final_weight >= delta;

	return result;
}

/*
 * Queries
 */

std::shared_ptr<nano::block> nano::election_ballot::find (nano::block_hash const & hash) const
{
	if (auto existing = last_blocks.find (hash); existing != last_blocks.end ())
	{
		return existing->second;
	}
	return nullptr;
}

bool nano::election_ballot::contains (nano::block_hash const & hash) const
{
	return last_blocks.contains (hash);
}

std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> nano::election_ballot::blocks () const
{
	return last_blocks;
}

std::unordered_set<nano::block_hash> nano::election_ballot::blocks_hashes () const
{
	std::unordered_set<nano::block_hash> result;
	for (auto const & [hash, block] : last_blocks)
	{
		result.emplace (hash);
	}
	return result;
}

std::unordered_map<nano::account, nano::vote_info> nano::election_ballot::votes () const
{
	return last_votes;
}

std::vector<nano::vote_with_weight_info> nano::election_ballot::votes_with_weight () const
{
	std::multimap<nano::uint128_t, nano::vote_with_weight_info, std::greater<nano::uint128_t>> sorted_votes;
	for (auto const & [account, info] : last_votes)
	{
		if (account != nullptr)
		{
			auto amount = weight (account);
			nano::vote_with_weight_info vote_info{ account, info.time, info.timestamp, info.hash, amount };
			sorted_votes.emplace (std::move (amount), vote_info);
		}
	}
	std::vector<nano::vote_with_weight_info> result;
	result.reserve (sorted_votes.size ());
	std::transform (sorted_votes.begin (), sorted_votes.end (), std::back_inserter (result), [] (auto const & entry) { return entry.second; });
	return result;
}

size_t nano::election_ballot::voter_count () const
{
	return last_votes.size ();
}

size_t nano::election_ballot::block_count () const
{
	return last_blocks.size ();
}
