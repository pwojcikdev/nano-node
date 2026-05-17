#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>

#include <boost/multiprecision/cpp_int.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace nano
{
class block;
}

namespace nano::consensus
{
// Same concrete type as nano::tally_t (nano/node/election.hpp). Declaring the alias here keeps the
// library free of node headers; both names denote the identical std::map specialization, so the
// adapter passes values across the boundary without conversion.
using tally_t = std::map<nano::uint128_t, std::shared_ptr<nano::block>, std::greater<nano::uint128_t>>;

// Per-representative vote state the core needs. The adapter keeps its own richer record (with the
// steady-clock time used for cooldown); the core only needs hash + timestamp.
struct vote_record final
{
	nano::block_hash hash;
	uint64_t timestamp;
};

// Injected pure queries. The library never includes node headers; all node coupling crosses here.
// Each call may return a different value and the core re-queries everywhere develop re-reads
// (live ledger weight, dynamic delta), which is how exact parity is achieved without coupling.
struct ports final
{
	std::function<nano::uint128_t (nano::account const &)> weight; // -> node.ledger.weight
	std::function<nano::uint128_t ()> delta; // -> node.online_reps.delta (uint256 math left in online_reps)
};

// Decisions the core returns; the adapter executes the side effects, in this order.
struct effects final
{
	// sum(candidate-restricted tally) >= delta && tally winner != current status winner.
	// Adapter: set status.winner, remove_votes(previous_winner) via the node.history handshake
	// (-> apply_removed_votes), block_processor.force(new_winner).
	struct winner_changed final
	{
		nano::block_hash previous_winner;
		std::shared_ptr<nano::block> new_winner;
	};
	// First time margin quorum is reached (one-shot, latched even if voting is disabled — matches
	// is_quorum.exchange). Adapter, iff enable_voting && reps().voting > 0: ++vote_broadcast_count,
	// vote_generator.vote_final(qualified_root, winner, bucket).
	struct quorum_reached final
	{
		nano::block_hash winner;
	};
	// Margin quorum AND the winner's final weight >= delta. The core has already set state =
	// confirmed. Adapter: block_processor.add(winner, election) then confirm_once.
	struct final_quorum_reached final
	{
		std::shared_ptr<nano::block> winner;
	};

	std::optional<winner_changed> on_winner_changed;
	std::optional<quorum_reached> on_quorum_reached;
	std::optional<final_quorum_reached> on_final_quorum_reached;
};

// Mirrors nano::election_state (nano/node/election.hpp) exactly.
enum class election_state
{
	passive,
	active,
	confirmed,
	expired_confirmed,
	expired_unconfirmed,
	cancelled,
};

// Pure ORV consensus core. Single-threaded and caller-serialized: it performs NO internal locking
// and is NOT thread-safe; the nano::election adapter holds its mutex across every call. The core
// receives only already-admitted votes for this election — routing (qualified_root matching),
// min-principal-weight, replay and cooldown stay in the adapter, unchanged from develop.
class election final
{
public:
	election (consensus::ports, std::shared_ptr<nano::block> const & initial_block);

	// Apply an already-admitted vote, then evaluate quorum from a single tally snapshot. Mirrors
	// the consensus portion of nano::election::vote + confirm_if_quorum (election.cpp:551-626,
	// 476-524). evaluate() is skipped once confirmed, matching `if (!confirmed_locked())`.
	void vote (nano::account const & representative, nano::block_hash const &, uint64_t timestamp, effects & out);

	// Consensus portion of nano::election::publish (election.cpp:628-667). The confirmed guard and
	// replace_by_weight eviction stay in the adapter. Returns true if the block already existed
	// (content replaced), false if newly added — same sense as the canonical `result`.
	bool publish (std::shared_ptr<nano::block> const &);

	// Winner-flip handshake: the adapter resolves node.history.votes(root, previous_winner) into
	// accounts and calls this to erase them (nano::election::remove_votes, election.cpp:738-752).
	void apply_removed_votes (std::vector<nano::account> const &);

	// nano::election::remove_block (election.cpp:754-769) minus the node-side filter/winner guard,
	// which stay in the adapter. Erases the block and its orphan votes.
	void remove_block (nano::block_hash const &);

	tally_t tally () const; // live weight, candidate-restricted; refreshes last_tally / final_weight
	nano::uint128_t final_tally () const; // winner-specific, sticky
	bool has_quorum () const; // margin rule over the current tally
	bool quorum_latched () const; // one-shot: has margin quorum ever been reached (sticky)

	// Aggregated weight per voted hash (ALL votes, not candidate-restricted) from the last tally().
	// Drives the adapter's replace_by_weight (election.cpp:777-779).
	std::unordered_map<nano::block_hash, nano::uint128_t> const & last_tally () const;

	nano::uint128_t status_tally () const; // tally winner weight from the last evaluate -> status.tally
	nano::uint128_t status_final_tally () const; // -> status.final_tally
	std::shared_ptr<nano::block> status_winner () const;

	election_state state () const;
	bool confirmed () const; // confirmed || expired_confirmed
	bool failed () const; // expired_unconfirmed
	// valid_change-gated (election.cpp:112-180). Returns true on FAILURE (no transition), false on
	// success — same inverted sense as nano::election::state_change.
	bool state_change (election_state expected, election_state desired);

	std::unordered_map<nano::account, vote_record> const & votes () const;
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> const & blocks () const;
	std::size_t voter_count () const; // includes the seeded null-account vote, matching develop
	std::size_t block_count () const;
	bool contains_block (nano::block_hash const &) const;
	bool contains_voter (nano::account const &) const;
	std::shared_ptr<nano::block> find (nano::block_hash const &) const;

private:
	bool has_quorum (tally_t const &) const;
	tally_t tally_impl () const;
	void evaluate (effects & out); // == confirm_if_quorum consensus body (election.cpp:476-524)
	bool valid_change (election_state, election_state) const;

	consensus::ports ports_;
	std::unordered_map<nano::account, vote_record> last_votes_;
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> last_blocks_;
	nano::block_hash status_winner_hash_;
	std::shared_ptr<nano::block> status_winner_block_;
	election_state state_{ election_state::passive };
	bool is_quorum_latched_{ false };
	mutable nano::uint128_t final_weight_{ 0 };
	mutable nano::uint128_t status_tally_{ 0 };
	mutable std::unordered_map<nano::block_hash, nano::uint128_t> last_tally_;
};
}
