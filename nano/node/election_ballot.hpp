#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/vote_with_weight_info.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nano
{
// A rep's most recent vote observed by one election
class vote_info final
{
public:
	std::chrono::steady_clock::time_point time{}; // Local arrival time, drives the cooldown window
	uint64_t timestamp{ 0 }; // Timestamp carried by the vote itself, the max value marks a final vote
	nano::block_hash hash{ 0 }; // Block the rep voted for
};

// Vote weight per block, ordered greatest weight first; equal weights keep the higher hash first, mirroring the vote tie-break rule
using tally_t = std::multimap<nano::uint128_t, std::shared_ptr<nano::block>, std::greater<nano::uint128_t>>;

// Minimum time between subsequent non-final votes from a rep of the given weight
std::chrono::seconds vote_cooldown (nano::uint128_t weight, nano::uint128_t online_stake);

/**
 * Vote and block bookkeeping for a single election: tracks each rep's latest vote and every
 * competing block, computes the weighted tally and quorum.
 * Pure logic with injected weight lookup and time, guarded externally by the owning election's mutex.
 */
class election_ballot final
{
public:
	using weight_fn = std::function<nano::uint128_t (nano::account const &)>;

	election_ballot (std::shared_ptr<nano::block> const & initial, weight_fn);

public: // Votes
	enum class vote_result
	{
		accepted, // Vote recorded
		replay, // Vote does not rank above the rep's recorded vote, ordered by timestamp then hash
		ignored, // Vote is valid but arrived within the rep's cooldown window
	};

	// Record a rep's vote, replacing its recorded vote. The hash may refer to a block this election does not hold yet, the vote is kept and tallied once the block arrives
	// Pass a `cooldown` of 0 to skip throttling (e.g. for cached votes)
	vote_result insert_vote (nano::account const & rep, uint64_t timestamp, nano::block_hash const &, std::chrono::steady_clock::time_point now, std::chrono::seconds cooldown);

	// Latest vote recorded for the rep, or a default-constructed vote_info when there is none
	nano::vote_info get_vote (nano::account const &) const;
	// Overwrite a rep's vote directly, bypassing admission and cooldown checks
	void set_vote (nano::account const &, nano::vote_info);
	// Drop the given reps' votes, used when the winning fork changes
	void erase_votes (std::vector<nano::account> const &);

public: // Blocks
	// Insert a competing block, or refresh the stored pointer if the hash is already present. Returns true if the block was newly added.
	// Does not enforce the block limit, callers are expected to check `full ()` and evict via `replacement_candidate` first
	bool insert_block (std::shared_ptr<nano::block> const &);

	// Weakest block to evict for a new one with the given inactive (vote cache) tally, never the current winner. Only nominated when the new tally outweighs it
	std::optional<nano::block_hash> replacement_candidate (nano::uint128_t inactive_tally, nano::block_hash const & winner) const;

	// Erase a block and every vote pointing at it, returning the erased block so the caller can clear network filters
	// Returns null and erases nothing when the hash is unknown or is the current winner
	std::shared_ptr<nano::block> erase_block (nano::block_hash const &, nano::block_hash const & winner);

	// At or above the maximum number of competing blocks
	bool full () const;

public: // Tally
	struct tally_result final
	{
		nano::tally_t tally; // Weight per block, greatest first
		std::shared_ptr<nano::block> winner; // Highest-tally block, null when no vote for a known block has been recorded yet
		nano::uint128_t winner_weight{ 0 }; // Weight behind the winner
		nano::uint128_t final_weight{ 0 }; // Final-vote weight behind the winner
		nano::uint128_t total_weight{ 0 }; // Sum of all tallied weight, excluding votes for blocks unknown to this election
		bool quorum{ false }; // Winner leads the runner-up by at least `delta`
		bool final_quorum{ false }; // Final-vote weight alone reaches `delta`
	};

	// Recompute the tally and evaluate quorum against the given online weight delta, refreshing the cached tally used for block replacement
	tally_result evaluate (nano::uint128_t delta);

	// Tally only, without refreshing the cached tally used for block replacement
	nano::tally_t tally () const;

public: // Queries
	std::shared_ptr<nano::block> find (nano::block_hash const &) const;
	bool contains (nano::block_hash const &) const;
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> blocks () const;
	std::unordered_set<nano::block_hash> blocks_hashes () const;
	std::unordered_map<nano::account, nano::vote_info> votes () const;
	// All recorded votes with the reps' current weights, heaviest rep first
	std::vector<nano::vote_with_weight_info> votes_with_weight () const;
	size_t voter_count () const;
	size_t block_count () const;

private:
	// Weight per block, and the final-vote weight per block, over the current votes
	struct weights
	{
		std::unordered_map<nano::block_hash, nano::uint128_t> block_weights;
		std::unordered_map<nano::block_hash, nano::uint128_t> final_weights;
	};

	weights compute_weights () const;
	// Order block weights into a tally, dropping weight behind blocks this election does not hold
	nano::tally_t make_tally (std::unordered_map<nano::block_hash, nano::uint128_t> const & block_weights) const;

private: // Dependencies
	weight_fn const weight;

private:
	// Every competing block by hash, including the initial block
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> last_blocks;
	// Latest vote per rep, may reference hashes not (yet) present in `last_blocks`
	std::unordered_map<nano::account, nano::vote_info> last_votes;
	// Weight per block as of the last `evaluate`, used to pick replacement candidates
	std::unordered_map<nano::block_hash, nano::uint128_t> last_tally;

	// Maximum number of competing blocks tracked per election
	static std::size_t constexpr max_blocks{ 10 };
};
}
