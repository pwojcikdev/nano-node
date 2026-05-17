#include <nano/consensus/consensus.hpp>
#include <nano/lib/assert.hpp>

nano::consensus::engine::engine (consensus::ports ports_a, nano::block_hash initial_candidate) :
	ports_{ std::move (ports_a) },
	state_{ no_quorum_state{ initial_candidate } }
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

nano::consensus::engine::assessment nano::consensus::engine::assess () const
{
	// One fresh tally reading drives every decision for this vote.
	auto tally_l = tally ();
	release_assert (!tally_l.empty ());
	winner_weight_ = tally_l.begin ()->first;
	nano::uint128_t sum{ 0 };
	for (auto const & [weight, hash] : tally_l)
	{
		sum += weight;
	}
	return assessment{ tally_l.begin ()->second, tally_l.begin ()->first, sum, has_quorum (tally_l), final_weight_ };
}

nano::block_hash nano::consensus::engine::relock (nano::block_hash locked, assessment const & a, effects & out) const
{
	// Only switch the locked winner once enough total weight has been cast (>= delta) AND the
	// leader is a different block. This prevents thrashing the winner on early, sparse votes.
	if (a.sum >= ports_.delta () && a.leading != locked)
	{
		out.on_winner_changed = effects::winner_changed{ locked, a.leading };
		return a.leading;
	}
	return locked;
}

auto nano::consensus::engine::vote_visitor::operator() (no_quorum_state const & s) const -> result
{
	effects out;
	auto a = engine_.assess ();
	auto winner = engine_.relock (s.winner, a, out);
	if (a.quorum)
	{
		// First time margin quorum is satisfied: latch by leaving no_quorum.
		out.on_quorum_reached = effects::quorum_reached{ winner };
		if (a.final_weight >= engine_.ports_.delta ())
		{
			out.on_final_quorum_reached = effects::final_quorum_reached{ a.leading };
			return { out, final_quorum_reached_state{ winner } };
		}
		return { out, quorum_reached_state{ winner } };
	}
	if (winner != s.winner)
	{
		return { out, no_quorum_state{ winner } }; // relocked but still no quorum
	}
	return { out, std::nullopt };
}

auto nano::consensus::engine::vote_visitor::operator() (quorum_reached_state const & s) const -> result
{
	effects out;
	auto a = engine_.assess ();
	auto winner = engine_.relock (s.winner, a, out);
	// Quorum is already latched, so it is never re-announced. The winner can still flip, and the
	// election is decided once the winner's final-vote weight also clears delta.
	if (a.quorum && a.final_weight >= engine_.ports_.delta ())
	{
		out.on_final_quorum_reached = effects::final_quorum_reached{ a.leading };
		return { out, final_quorum_reached_state{ winner } };
	}
	if (winner != s.winner)
	{
		return { out, quorum_reached_state{ winner } };
	}
	return { out, std::nullopt };
}

auto nano::consensus::engine::vote_visitor::operator() (final_quorum_reached_state const &) const -> result
{
	return { {}, std::nullopt }; // decided; further votes are ignored
}

nano::consensus::effects nano::consensus::engine::vote (nano::account const & representative, nano::block_hash const & hash, uint64_t timestamp)
{
	// The caller already enforced replay/cooldown/minimum-principal-weight; an accepted ballot
	// unconditionally replaces this representative's previous one.
	votes_[representative] = ballot{ hash, timestamp };
	auto [out, next] = std::visit (vote_visitor{ *this }, state_);
	if (next)
	{
		state_ = std::move (*next);
	}
	return out;
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
	if (winner () != hash_a && candidates_.erase (hash_a) > 0)
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
	return std::visit ([] (auto const & s) { return s.winner; }, state_);
}

nano::consensus::election_phase nano::consensus::engine::phase () const
{
	return std::visit ([] (auto const & s) { return s.phase; }, state_);
}

bool nano::consensus::engine::confirmed () const
{
	return std::holds_alternative<final_quorum_reached_state> (state_);
}

bool nano::consensus::engine::quorum_latched () const
{
	return !std::holds_alternative<no_quorum_state> (state_);
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
