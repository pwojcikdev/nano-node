#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>

#include <boost/multiprecision/cpp_int.hpp>

#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nano::consensus
{
// Weighted tally, highest weight first. Keyed by weight: two blocks with exactly equal weight
// collide on the same key and only the first inserted survives. That collapse is an intentional,
// preserved property of the rule, not an accident — every consumer sees a single block per
// weight. The engine works purely in block hashes; resolving a hash to a block is the caller's
// job.
using tally_t = std::map<nano::uint128_t, nano::block_hash, std::greater<nano::uint128_t>>;

// One representative's current ballot in an election. A representative has at most one ballot at
// a time; a newer ballot replaces the older one. The sentinel timestamp marks a final
// (irreversible) vote — only final votes count toward final-quorum confirmation.
struct ballot final
{
	nano::block_hash hash;
	uint64_t timestamp;

	static uint64_t constexpr final_timestamp = std::numeric_limits<uint64_t>::max ();
};

// Injected pure queries. The engine never includes node headers; all node coupling crosses this
// boundary. Each query is re-invoked every time the rule re-reads it, so the engine always sees
// live representative weight and the current online quorum delta without depending on the node.
struct ports final
{
	std::function<nano::uint128_t (nano::account const &)> weight; // a representative's voting weight
	std::function<nano::uint128_t ()> delta; // the current online quorum delta
};

// Decisions the engine produces from a vote; the caller performs the side effects in this order.
// Each is engaged at most once per vote.
struct effects final
{
	// Total weight cast across candidates reached delta and the leading block is no longer the
	// locked winner, so the engine relocked onto new_winner. The caller forgets the previous
	// winner's generated votes and forces the new winner block into processing.
	struct winner_changed final
	{
		nano::block_hash previous_winner;
		nano::block_hash new_winner;
	};
	// Margin quorum was reached for the first time. Latched: fires exactly once for the lifetime
	// of the election, even if voting is disabled and even before any final votes arrive.
	struct quorum_reached final
	{
		nano::block_hash winner;
	};
	// The winner additionally reached quorum among final votes: the election is decided and the
	// engine has moved to the confirmed state. The caller cements the winner.
	struct final_quorum_reached final
	{
		nano::block_hash winner;
	};

	std::optional<winner_changed> on_winner_changed;
	std::optional<quorum_reached> on_quorum_reached;
	std::optional<final_quorum_reached> on_final_quorum_reached;
};

enum class election_state
{
	passive,
	active,
	confirmed,
	expired_confirmed,
	expired_unconfirmed,
	cancelled,
};

// The ORV (Open Representative Voting) decision engine for a single election. It owns the per-
// representative ballots and the candidate hash set, computes the weighted tally, and drives the
// no-quorum -> quorum -> final-quorum progression, returning the decisions the caller must act
// on. It is the canonical implementation of these rules.
//
// Single-threaded and caller-serialized: it performs NO internal locking and is NOT thread-safe;
// the caller holds its own mutex across every call. It only ever sees votes already admitted for
// this election — vote routing, minimum-principal-weight filtering, replay and cooldown are the
// caller's responsibility and are deliberately outside the consensus rule.
class engine final
{
public:
	engine (consensus::ports, nano::block_hash initial_candidate);

	// Record an already-admitted ballot for a representative (replacing any previous one), then,
	// unless the election is already confirmed, re-evaluate the quorum progression from a single
	// fresh tally snapshot and return the resulting decisions.
	effects vote (nano::account const & representative, nano::block_hash const &, uint64_t timestamp);

	// Make a block hash eligible to appear in the tally. Returns true if it was newly added.
	bool register_candidate (nano::block_hash const &);

	// Drop a candidate (never the locked winner) and any ballots that pointed at it.
	void remove_candidate (nano::block_hash const &);

	// Winner-flip handshake: when the winner changes the caller resolves which representatives'
	// generated votes were discarded and asks the engine to forget those ballots.
	void forget_voters (std::vector<nano::account> const &);

	tally_t tally () const; // live weighted tally, restricted to candidates; refreshes the snapshots below
	nano::uint128_t winner_weight () const; // weight of the leading block at the last evaluate
	nano::uint128_t final_weight () const; // winner's final-vote weight; sticky (see tally())
	bool has_quorum () const; // does the current tally satisfy the margin rule
	bool quorum_latched () const; // sticky one-shot: has the margin rule ever been satisfied
	nano::block_hash winner () const; // the currently locked winner hash

	// Total weight per voted hash across ALL ballots (not candidate-restricted) from the last
	// tally(). The caller uses this to decide block eviction.
	std::unordered_map<nano::block_hash, nano::uint128_t> const & tally_snapshot () const;

	election_state state () const;
	bool confirmed () const; // confirmed || expired_confirmed
	bool failed () const; // expired_unconfirmed
	// Attempt expected -> desired if the transition is valid and the current state matches.
	// Returns true on FAILURE (no transition), false on success.
	bool state_change (election_state expected, election_state desired);

	std::unordered_map<nano::account, ballot> const & votes () const;
	std::unordered_set<nano::block_hash> const & candidates () const;
	std::size_t voter_count () const; // includes the seeded null-account ballot
	std::size_t candidate_count () const;
	bool contains_candidate (nano::block_hash const &) const;
	bool contains_voter (nano::account const &) const;

private:
	bool has_quorum (tally_t const &) const;
	effects evaluate ();
	bool valid_change (election_state, election_state) const;

	consensus::ports ports_;
	std::unordered_map<nano::account, ballot> votes_;
	std::unordered_set<nano::block_hash> candidates_;
	nano::block_hash winner_hash_;
	election_state state_{ election_state::passive };
	bool quorum_latched_{ false };
	mutable nano::uint128_t final_weight_{ 0 };
	mutable nano::uint128_t winner_weight_{ 0 };
	mutable std::unordered_map<nano::block_hash, nano::uint128_t> tally_snapshot_;
};
}
