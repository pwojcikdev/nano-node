#include <nano/consensus/consensus.hpp>
#include <nano/lib/assert.hpp>
#include <nano/lib/blocks.hpp>

#include <limits>

nano::consensus::election::election (consensus::ports ports_a, std::shared_ptr<nano::block> const & initial_block) :
	ports_{ std::move (ports_a) },
	status_winner_hash_{ initial_block->hash () },
	status_winner_block_{ initial_block }
{
	// Mirrors nano::election ctor (election.cpp:46-47): a null-account vote pins the initial block
	// into the tally so the election always has a candidate.
	last_votes_.emplace (nano::account::null (), vote_record{ initial_block->hash (), 0 });
	last_blocks_.emplace (initial_block->hash (), initial_block);
}

nano::consensus::tally_t nano::consensus::election::tally_impl () const
{
	// Verbatim parity with nano::election::tally_impl (election.cpp:440-474): live weight per
	// voter, last_tally snapshots ALL voted hashes, the ordered tally is candidate-restricted, and
	// final_weight is the WINNER's final weight and is sticky (left unchanged otherwise).
	std::unordered_map<nano::block_hash, nano::uint128_t> block_weights;
	std::unordered_map<nano::block_hash, nano::uint128_t> final_weights_l;
	for (auto const & [account, info] : last_votes_)
	{
		auto rep_weight = ports_.weight (account);
		block_weights[info.hash] += rep_weight;
		if (info.timestamp == std::numeric_limits<uint64_t>::max ())
		{
			final_weights_l[info.hash] += rep_weight;
		}
	}
	last_tally_ = block_weights;
	consensus::tally_t result;
	for (auto const & [hash, amount] : block_weights)
	{
		auto block = last_blocks_.find (hash);
		if (block != last_blocks_.end ())
		{
			result.emplace (amount, block->second);
		}
	}
	if (!final_weights_l.empty () && !result.empty ())
	{
		auto winner_hash = result.begin ()->second->hash ();
		auto find_final = final_weights_l.find (winner_hash);
		if (find_final != final_weights_l.end ())
		{
			final_weight_ = find_final->second;
		}
	}
	return result;
}

bool nano::consensus::election::has_quorum (consensus::tally_t const & tally_a) const
{
	// Verbatim parity with nano::election::have_quorum (election.cpp:423-432): the MARGIN over the
	// runner-up must reach delta, NOT an absolute threshold.
	auto i = tally_a.begin ();
	++i;
	auto second = (i != tally_a.end () ? i->first : nano::uint128_t{ 0 });
	auto delta_l = ports_.delta ();
	release_assert (tally_a.begin ()->first >= second);
	return (tally_a.begin ()->first - second) >= delta_l;
}

bool nano::consensus::election::has_quorum () const
{
	auto tally_l = tally_impl ();
	if (tally_l.empty ())
	{
		return false;
	}
	return has_quorum (tally_l);
}

bool nano::consensus::election::quorum_latched () const
{
	return is_quorum_latched_;
}

void nano::consensus::election::evaluate (effects & out)
{
	// Verbatim parity with nano::election::confirm_if_quorum (election.cpp:476-524). The single
	// tally snapshot taken here drives every decision below, so a winner flip's vote removal (done
	// by the adapter via apply_removed_votes) only affects the NEXT evaluate — exactly as develop,
	// where remove_votes runs after tally_l is computed.
	auto tally_l = tally_impl ();
	release_assert (!tally_l.empty ());
	auto winner = tally_l.begin ();
	auto block_l = winner->second;
	auto const & winner_hash_l = block_l->hash ();
	status_tally_ = winner->first;
	// status.final_tally is set from final_weight_ by the adapter via status_final_tally().

	nano::uint128_t sum{ 0 };
	for (auto const & i : tally_l)
	{
		sum += i.first;
	}
	if (sum >= ports_.delta () && winner_hash_l != status_winner_hash_)
	{
		auto previous = status_winner_hash_;
		status_winner_hash_ = winner_hash_l;
		status_winner_block_ = block_l;
		out.on_winner_changed = effects::winner_changed{ previous, block_l };
	}
	if (has_quorum (tally_l))
	{
		if (!is_quorum_latched_)
		{
			is_quorum_latched_ = true;
			out.on_quorum_reached = effects::quorum_reached{ status_winner_hash_ };
		}
		if (final_weight_ >= ports_.delta ())
		{
			state_ = election_state::confirmed;
			out.on_final_quorum_reached = effects::final_quorum_reached{ block_l };
		}
	}
}

void nano::consensus::election::vote (nano::account const & representative, nano::block_hash const & hash, uint64_t timestamp, effects & out)
{
	// The adapter already enforced replay/cooldown/min-principal; the accepted vote overwrites any
	// previous entry for this representative (election.cpp:591).
	last_votes_[representative] = vote_record{ hash, timestamp };
	if (!confirmed ())
	{
		evaluate (out);
	}
}

bool nano::consensus::election::publish (std::shared_ptr<nano::block> const & block_a)
{
	// Consensus portion of nano::election::publish (election.cpp:643-659). The confirmed guard and
	// replace_by_weight eviction are the adapter's responsibility.
	bool result = false;
	auto existing = last_blocks_.find (block_a->hash ());
	if (existing == last_blocks_.end ())
	{
		last_blocks_.emplace (block_a->hash (), block_a);
	}
	else
	{
		result = true;
		existing->second = block_a;
		if (status_winner_hash_ == block_a->hash ())
		{
			status_winner_block_ = block_a;
		}
	}
	return result;
}

void nano::consensus::election::apply_removed_votes (std::vector<nano::account> const & accounts)
{
	for (auto const & account : accounts)
	{
		last_votes_.erase (account);
	}
}

void nano::consensus::election::remove_block (nano::block_hash const & hash_a)
{
	// nano::election::remove_block (election.cpp:754-769); the winner guard is consensus-relevant
	// and stays here, network.filter.clear stays in the adapter.
	if (status_winner_hash_ != hash_a)
	{
		if (auto existing = last_blocks_.find (hash_a); existing != last_blocks_.end ())
		{
			std::erase_if (last_votes_, [&hash_a] (auto const & entry) {
				return entry.second.hash == hash_a;
			});
			last_blocks_.erase (hash_a);
		}
	}
}

nano::consensus::tally_t nano::consensus::election::tally () const
{
	return tally_impl ();
}

nano::uint128_t nano::consensus::election::final_tally () const
{
	return final_weight_;
}

std::unordered_map<nano::block_hash, nano::uint128_t> const & nano::consensus::election::last_tally () const
{
	return last_tally_;
}

nano::uint128_t nano::consensus::election::status_tally () const
{
	return status_tally_;
}

nano::uint128_t nano::consensus::election::status_final_tally () const
{
	return final_weight_;
}

std::shared_ptr<nano::block> nano::consensus::election::status_winner () const
{
	return status_winner_block_;
}

nano::consensus::election_state nano::consensus::election::state () const
{
	return state_;
}

bool nano::consensus::election::confirmed () const
{
	return state_ == election_state::confirmed || state_ == election_state::expired_confirmed;
}

bool nano::consensus::election::failed () const
{
	return state_ == election_state::expired_unconfirmed;
}

bool nano::consensus::election::valid_change (election_state expected_a, election_state desired_a) const
{
	// Verbatim parity with nano::election::valid_change (election.cpp:112-155).
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

bool nano::consensus::election::state_change (election_state expected_a, election_state desired_a)
{
	// Parity with nano::election::state_change (election.cpp:157-178): returns true on failure
	// (no transition). The state_start timestamp and update_action posting stay in the adapter.
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

std::unordered_map<nano::account, nano::consensus::vote_record> const & nano::consensus::election::votes () const
{
	return last_votes_;
}

std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> const & nano::consensus::election::blocks () const
{
	return last_blocks_;
}

std::size_t nano::consensus::election::voter_count () const
{
	return last_votes_.size ();
}

std::size_t nano::consensus::election::block_count () const
{
	return last_blocks_.size ();
}

bool nano::consensus::election::contains_block (nano::block_hash const & hash_a) const
{
	return last_blocks_.find (hash_a) != last_blocks_.end ();
}

bool nano::consensus::election::contains_voter (nano::account const & account_a) const
{
	return last_votes_.find (account_a) != last_votes_.end ();
}

std::shared_ptr<nano::block> nano::consensus::election::find (nano::block_hash const & hash_a) const
{
	if (auto existing = last_blocks_.find (hash_a); existing != last_blocks_.end ())
	{
		return existing->second;
	}
	return nullptr;
}
