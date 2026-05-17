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
#include <variant>
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
// Each is engaged at most once per vote. Note "leader" (the block this node currently backs) vs
// "winner" (the block that actually won the election): only on_final_quorum_reached carries a
// winner, because that is the only point at which the election is decided.
struct effects final
{
	// Total weight cast across candidates reached delta and the tally leader is no longer the
	// block this node had locked, so the engine relocked onto new_leader. The caller forgets the
	// previous leader's generated votes and forces the new leader block into processing.
	struct leader_changed final
	{
		nano::block_hash previous_leader;
		nano::block_hash new_leader;
	};
	// Margin quorum was reached for the first time (the no_quorum -> quorum_reached transition).
	// Fires exactly once per election, even if voting is disabled and even before final votes.
	struct quorum_reached final
	{
		nano::block_hash leader;
	};
	// The leader additionally reached quorum among final votes: the election is decided and the
	// engine has moved to the final_quorum_reached phase. The caller cements the winner.
	struct final_quorum_reached final
	{
		nano::block_hash winner;
	};

	std::optional<leader_changed> on_leader_changed;
	std::optional<quorum_reached> on_quorum_reached;
	std::optional<final_quorum_reached> on_final_quorum_reached;
};

// The three phases of an ORV decision. This is the engine's entire state — it deliberately does
// NOT model the node-side election lifecycle (passive/active/expired/cancelled); that belongs to
// the caller.
enum class election_phase
{
	no_quorum, // no margin quorum yet; the locked leader may still flip
	quorum_reached, // margin quorum latched; still waiting on final votes
	final_quorum_reached, // decided; further votes are ignored
};

// The ORV (Open Representative Voting) decision engine for a single election. It owns the per-
// representative ballots and the candidate hash set, computes the weighted tally, and walks the
// no_quorum -> quorum_reached -> final_quorum_reached progression, returning the decisions the
// caller must act on. It is the canonical implementation of these rules and decides only — vote
// generation and all side effects stay with the caller.
//
// Single-threaded and caller-serialized: it performs NO internal locking and is NOT thread-safe;
// the caller holds its own mutex across every call. It only ever sees votes already admitted for
// this election — vote routing, minimum-principal-weight filtering, replay and cooldown are the
// caller's responsibility and are deliberately outside the consensus rule.
class engine final
{
public:
	engine (consensus::ports, nano::block_hash initial_candidate);

	// Record an already-admitted ballot for a representative (replacing any previous one) and let
	// the current phase decide what changes, returning the resulting decisions. Votes arriving
	// after the election is decided are recorded but produce no decisions.
	effects vote (nano::account const & representative, nano::block_hash const &, uint64_t timestamp);

	// Make a block hash eligible to appear in the tally. Returns true if it was newly added.
	bool register_candidate (nano::block_hash const &);

	// Drop a candidate (never the current leader) and any ballots that pointed at it.
	void remove_candidate (nano::block_hash const &);

	// Leader-flip handshake: when the leader changes the caller resolves which representatives'
	// generated votes were discarded and asks the engine to forget those ballots.
	void forget_voters (std::vector<nano::account> const &);

	// All read-only views are pure functions of the current ballots, candidates and injected
	// weights — the engine caches nothing.
	tally_t tally () const; // weighted tally, restricted to candidates, highest first
	nano::uint128_t leader_weight () const; // weight of the current leading block
	nano::uint128_t final_weight () const; // the leading block's current final-vote weight (0 if none)
	bool has_quorum () const; // does the current tally satisfy the margin rule
	bool quorum_latched () const; // sticky one-shot: has the margin rule ever been satisfied
	nano::block_hash leader () const; // the block this node currently backs (always present)
	std::optional<nano::block_hash> winner () const; // the decided block; empty until final quorum

	// Weight per voted hash across ALL ballots (not candidate-restricted). The caller uses this
	// to decide block eviction. Computed on demand.
	std::unordered_map<nano::block_hash, nano::uint128_t> ballot_weights () const;

	election_phase phase () const;
	bool confirmed () const; // phase == final_quorum_reached

	std::unordered_map<nano::account, ballot> const & votes () const;
	std::unordered_set<nano::block_hash> const & candidates () const;
	std::size_t voter_count () const; // includes the seeded null-account ballot
	std::size_t candidate_count () const;
	bool contains_candidate (nano::block_hash const &) const;
	bool contains_voter (nano::account const &) const;

private:
	// Each phase carries exactly the data meaningful in it — the backed `leader` while still
	// undecided, the decided `winner` once final quorum is reached — so the block can never drift
	// out of sync with the phase.
	struct no_quorum_state
	{
		static election_phase constexpr phase = election_phase::no_quorum;
		nano::block_hash leader;
	};
	struct quorum_reached_state
	{
		static election_phase constexpr phase = election_phase::quorum_reached;
		nano::block_hash leader;
	};
	struct final_quorum_reached_state
	{
		static election_phase constexpr phase = election_phase::final_quorum_reached;
		nano::block_hash winner; // decided: the leader that won is now the winner
	};
	using state_variant = std::variant<no_quorum_state, quorum_reached_state, final_quorum_reached_state>;

	// A single fresh reading of the tally that every per-phase decision is taken from. Purely
	// derived from the current ballots — nothing here is carried between votes.
	struct assessment
	{
		nano::block_hash leading; // highest-weight candidate
		nano::uint128_t leading_weight;
		nano::uint128_t sum; // total candidate weight
		bool quorum; // margin rule satisfied right now
		nano::uint128_t final_weight; // leading block's current final-vote weight (0 if it has none)
	};

	// Per-phase vote behavior. Returns the decisions plus, if the phase or locked leader changed,
	// the next state. Defined out-of-line in consensus.cpp.
	struct vote_visitor
	{
		engine const & engine_;
		using result = std::pair<effects, std::optional<state_variant>>;
		result operator() (no_quorum_state const &) const;
		result operator() (quorum_reached_state const &) const;
		result operator() (final_quorum_reached_state const &) const;
	};

	assessment assess () const;
	bool has_quorum (tally_t const &) const;
	// Sum-gated relock shared by the no_quorum and quorum_reached phases: if enough total weight
	// has been cast and the tally leader differs from the locked one, relock and emit the change.
	nano::block_hash relock (nano::block_hash locked, assessment const &, effects &) const;

	consensus::ports ports_;
	std::unordered_map<nano::account, ballot> votes_;
	std::unordered_set<nano::block_hash> candidates_;
	state_variant state_;
};
}
