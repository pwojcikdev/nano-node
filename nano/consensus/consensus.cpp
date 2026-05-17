#include <nano/consensus/consensus.hpp>
#include <nano/lib/assert.hpp>

nano::consensus::engine::engine (consensus::ports ports_a, nano::block_hash initial_candidate) :
	ports_{ std::move (ports_a) },
	winner_hash_{ initial_candidate }
{
	// Seed a zero-weight ballot from the null account pointing at the initial candidate. The null
	// account never has weight, so this only pins the candidate into the tally — the election
	// always has at least one candidate to lead, even before any real vote arrives.
	votes_.emplace (nano::account::null (), ballot{ initial_candidate, 0 });
	candidates_.insert (initial_candidate);
}

nano::consensus::tally_t nano::consensus::engine::tally () const
{
	// Sum live representative weight per voted hash. Two snapshots are produced:
	//  - tally_snapshot_: weight per hash across ALL ballots (used by the caller for eviction).
	//  - the returned tally: restricted to registered candidates and ordered by weight.
	// final_weight_ is the winner's weight among final votes only, and is STICKY: it is updated
	// solely when the current winner has final votes, so a transient winner flip can never lower
	// an already-reached final tally (which would otherwise un-confirm a decided election).
	std::unordered_map<nano::block_hash, nano::uint128_t> block_weights;
	std::unordered_map<nano::block_hash, nano::uint128_t> final_weights;
	for (auto const & [account, ballot] : votes_)
	{
		auto weight = ports_.weight (account);
		block_weights[ballot.hash] += weight;
		if (ballot.timestamp == ballot::final_timestamp)
		{
			final_weights[ballot.hash] += weight;
		}
	}
	tally_snapshot_ = block_weights;
	consensus::tally_t result;
	for (auto const & [hash, weight] : block_weights)
	{
		if (candidates_.contains (hash))
		{
			result.emplace (weight, hash);
		}
	}
	if (!final_weights.empty () && !result.empty ())
	{
		auto winner_hash = result.begin ()->second;
		if (auto it = final_weights.find (winner_hash); it != final_weights.end ())
		{
			final_weight_ = it->second;
		}
	}
	return result;
}

bool nano::consensus::engine::has_quorum (consensus::tally_t const & tally_a) const
{
	// The margin rule: the leader must beat the runner-up by at least delta. This is NOT an
	// absolute "leader >= delta" threshold — requiring a margin over the next strongest fork is
	// what makes confirmation safe in the presence of competing blocks.
	auto i = tally_a.begin ();
	++i;
	auto second = (i != tally_a.end () ? i->first : nano::uint128_t{ 0 });
	release_assert (tally_a.begin ()->first >= second);
	return (tally_a.begin ()->first - second) >= ports_.delta ();
}

bool nano::consensus::engine::has_quorum () const
{
	auto tally_l = tally ();
	if (tally_l.empty ())
	{
		return false;
	}
	return has_quorum (tally_l);
}

bool nano::consensus::engine::quorum_latched () const
{
	return quorum_latched_;
}

nano::consensus::effects nano::consensus::engine::evaluate ()
{
	// Every decision below is taken from this ONE tally snapshot. A winner flip's vote removal is
	// performed by the caller afterwards (forget_voters), so it can only affect the NEXT
	// evaluate — never the quorum check within this one.
	effects out;
	auto tally_l = tally ();
	release_assert (!tally_l.empty ());
	auto leading_hash = tally_l.begin ()->second;
	winner_weight_ = tally_l.begin ()->first;

	// Sum-gated relock: only switch the locked winner once enough total weight has been cast
	// (>= delta) AND the leader is a different block. This prevents thrashing the winner on
	// early, sparse votes.
	nano::uint128_t sum{ 0 };
	for (auto const & [weight, hash] : tally_l)
	{
		sum += weight;
	}
	if (sum >= ports_.delta () && leading_hash != winner_hash_)
	{
		auto previous = winner_hash_;
		winner_hash_ = leading_hash;
		out.on_winner_changed = effects::winner_changed{ previous, leading_hash };
	}
	if (has_quorum (tally_l))
	{
		// One-shot: latch on the first margin quorum so the caller broadcasts final votes exactly
		// once. The latch flips regardless of voting being enabled.
		if (!quorum_latched_)
		{
			quorum_latched_ = true;
			out.on_quorum_reached = effects::quorum_reached{ winner_hash_ };
		}
		// Decided once the winner's final-vote weight also clears delta.
		if (final_weight_ >= ports_.delta ())
		{
			state_ = election_state::confirmed;
			out.on_final_quorum_reached = effects::final_quorum_reached{ leading_hash };
		}
	}
	return out;
}

nano::consensus::effects nano::consensus::engine::vote (nano::account const & representative, nano::block_hash const & hash, uint64_t timestamp)
{
	// The caller already enforced replay/cooldown/minimum-principal-weight; an accepted ballot
	// unconditionally replaces this representative's previous one.
	votes_[representative] = ballot{ hash, timestamp };
	if (confirmed ())
	{
		return {};
	}
	return evaluate ();
}

bool nano::consensus::engine::register_candidate (nano::block_hash const & hash_a)
{
	return candidates_.insert (hash_a).second;
}

void nano::consensus::engine::forget_voters (std::vector<nano::account> const & accounts)
{
	for (auto const & account : accounts)
	{
		votes_.erase (account);
	}
}

void nano::consensus::engine::remove_candidate (nano::block_hash const & hash_a)
{
	// The locked winner is never removed. Removing a candidate also drops the now-orphaned
	// ballots that pointed at it so they stop contributing weight.
	if (winner_hash_ != hash_a && candidates_.erase (hash_a) > 0)
	{
		std::erase_if (votes_, [&hash_a] (auto const & entry) {
			return entry.second.hash == hash_a;
		});
	}
}

nano::uint128_t nano::consensus::engine::winner_weight () const
{
	return winner_weight_;
}

nano::uint128_t nano::consensus::engine::final_weight () const
{
	return final_weight_;
}

std::unordered_map<nano::block_hash, nano::uint128_t> const & nano::consensus::engine::tally_snapshot () const
{
	return tally_snapshot_;
}

nano::block_hash nano::consensus::engine::winner () const
{
	return winner_hash_;
}

nano::consensus::election_state nano::consensus::engine::state () const
{
	return state_;
}

bool nano::consensus::engine::confirmed () const
{
	return state_ == election_state::confirmed || state_ == election_state::expired_confirmed;
}

bool nano::consensus::engine::failed () const
{
	return state_ == election_state::expired_unconfirmed;
}

bool nano::consensus::engine::valid_change (election_state expected_a, election_state desired_a) const
{
	switch (expected_a)
	{
		case election_state::passive:
			switch (desired_a)
			{
				case election_state::active:
				case election_state::confirmed:
				case election_state::expired_unconfirmed:
				case election_state::cancelled:
					return true;
				default:
					break;
			}
			break;
		case election_state::active:
			switch (desired_a)
			{
				case election_state::confirmed:
				case election_state::expired_unconfirmed:
				case election_state::cancelled:
					return true;
				default:
					break;
			}
			break;
		case election_state::confirmed:
			switch (desired_a)
			{
				case election_state::expired_confirmed:
					return true;
				default:
					break;
			}
			break;
		case election_state::expired_unconfirmed:
		case election_state::expired_confirmed:
		case election_state::cancelled:
			break;
	}
	return false;
}

bool nano::consensus::engine::state_change (election_state expected_a, election_state desired_a)
{
	// Returns true on failure (no transition). The wall-clock state-start timestamp and any
	// notification posting are caller concerns and stay outside the rule.
	bool result = true;
	if (valid_change (expected_a, desired_a))
	{
		if (state_ == expected_a)
		{
			state_ = desired_a;
			result = false;
		}
	}
	return result;
}

std::unordered_map<nano::account, nano::consensus::ballot> const & nano::consensus::engine::votes () const
{
	return votes_;
}

std::unordered_set<nano::block_hash> const & nano::consensus::engine::candidates () const
{
	return candidates_;
}

std::size_t nano::consensus::engine::voter_count () const
{
	return votes_.size ();
}

std::size_t nano::consensus::engine::candidate_count () const
{
	return candidates_.size ();
}

bool nano::consensus::engine::contains_candidate (nano::block_hash const & hash_a) const
{
	return candidates_.contains (hash_a);
}

bool nano::consensus::engine::contains_voter (nano::account const & account_a) const
{
	return votes_.contains (account_a);
}
