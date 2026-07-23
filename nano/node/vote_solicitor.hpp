#pragma once

#include <nano/lib/locks.hpp>
#include <nano/node/election.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/repcrawler.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nano
{
/**
 * Voting state and work flags for a single election within a solicitation round.
 */
struct solicitation final
{
	std::shared_ptr<nano::block> winner;
	std::unordered_map<nano::account, nano::vote_info> votes;
	bool quorum{ false }; // Once quorum is reached only final votes are conclusive
	bool request{ false }; // Confirmation requests are due
	bool broadcast{ false }; // Winner block broadcast is due
};

/**
 * Plans one round of election solicitation: which representatives to request votes from and which to send the winner block to.
 * Pure bookkeeping without I/O or locking, the vote_solicitor component sends what a round accumulates.
 */
class vote_solicitor_round final
{
public:
	// Fork decisions (rep voted for a different hash) are exempt from the per election caps
	enum class rep_decision
	{
		skip,
		solicit,
		solicit_fork,
	};

	// Whether a confirmation request to this rep is warranted
	static rep_decision decide_request (nano::solicitation const &, nano::account const & rep);
	// Whether a winner block broadcast to this rep is warranted
	static rep_decision decide_broadcast (nano::solicitation const &, nano::account const & rep);

public:
	struct limits
	{
		std::size_t max_block_broadcasts{ 30 }; // Global per round cap on winner block broadcasts
		std::size_t max_election_requests{ 50 }; // Per election cap on confirmation requests, fork decisions are exempt
		std::size_t max_election_broadcasts{ 15 }; // Per election cap on directed broadcasts, fork decisions are exempt
	};

	struct result
	{
		bool requested{ false };
		bool broadcasted{ false };
	};

	vote_solicitor_round (std::vector<nano::representative> const & reps, limits const &);

	// Plan requests and broadcasts for a single solicitation
	result add (nano::solicitation const &);

	using roots_hashes_t = std::vector<std::pair<nano::block_hash, nano::root>>;
	using channels_t = std::vector<std::shared_ptr<nano::transport::channel>>;

	// Planned confirmation requests per channel
	std::unordered_map<std::shared_ptr<nano::transport::channel>, roots_hashes_t> const & requests () const;
	// Planned winner broadcasts with their target channels
	std::vector<std::pair<std::shared_ptr<nano::block>, channels_t>> const & broadcasts () const;

public:
	// Queried before planning a request on a channel, allows tests to simulate backpressure
	std::function<bool (std::shared_ptr<nano::transport::channel> const &)> channel_full{ [] (auto const &) { return false; } };

private:
	bool add_requests (nano::solicitation const &);
	bool add_broadcasts (nano::solicitation const &);

private:
	limits const limits_m;

	std::vector<nano::representative> reps_requests; // Reps with a full channel are dropped for the rest of the round
	std::vector<nano::representative> const reps_broadcasts;

	std::size_t block_broadcasts{ 0 };

	std::unordered_map<std::shared_ptr<nano::transport::channel>, roots_hashes_t> requests_m;
	std::vector<std::pair<std::shared_ptr<nano::block>, channels_t>> broadcasts_m;
};

/**
 * Solicits votes for active elections: batches confirmation requests to representatives and directs winner block broadcasts to those that have not voted.
 * Elections are registered with trigger () and evaluated in fixed cadence rounds on a dedicated thread.
 * Owns all solicitation pacing per election root, elections only provide voting state snapshots.
 */
class vote_solicitor final
{
public:
	// Per election solicitation pacing, advanced only for work actually performed
	struct pacing
	{
		std::chrono::steady_clock::time_point last_request{};
		std::chrono::steady_clock::time_point last_broadcast{};
		nano::block_hash last_broadcast_hash{ 0 };
	};

	struct due
	{
		bool request{ false };
		bool broadcast{ false };
	};

	// Interval between confirmation requests, scales with election behavior
	static std::chrono::milliseconds request_interval (nano::election_behavior, std::chrono::milliseconds base_latency);
	// Decide what solicitation work is due for an election
	static due decide (nano::election_snapshot const &, pacing const &, std::chrono::milliseconds base_latency, std::chrono::milliseconds broadcast_interval, std::chrono::steady_clock::time_point now);

public:
	vote_solicitor (nano::network &, nano::rep_crawler &, nano::block_rebroadcaster &, nano::network_constants const &, nano::stats &, nano::logger &);
	~vote_solicitor ();

	void start ();
	void stop ();

	// Register an election for solicitation, evaluated on the next round
	// Repeated triggers for the same root are coalesced
	void trigger (std::shared_ptr<nano::election> const &);

	using result = vote_solicitor_round::result;

	// Plan and send solicitations for one round, synchronous building block used by the round thread
	// @return results parallel to the input order
	std::vector<result> solicit (std::vector<nano::solicitation> const &) const;

	std::size_t size () const;
	bool empty () const;

	nano::container_info container_info () const;

public:
	// Limits for each planning round, the per election broadcast cap is derived from network fanout
	vote_solicitor_round::limits const limits;
	// Interval between solicitation rounds, keeps the per round caps meaningful
	std::chrono::milliseconds const round_interval;
	// Matches election::base_latency, used to scale request intervals
	std::chrono::milliseconds const base_latency;
	// Minimum interval between winner broadcasts for the same election
	std::chrono::milliseconds const broadcast_interval;

private:
	struct entry
	{
		std::weak_ptr<nano::election> election;
		std::shared_ptr<nano::election> pending; // Set by trigger, cleared once processed; keeps the election alive for its final broadcast round
		nano::vote_solicitor::pacing pacing;
	};

	struct round_item
	{
		nano::qualified_root root;
		std::shared_ptr<nano::election> election;
		nano::vote_solicitor::pacing pacing;
	};

	void run ();
	void run_round (std::deque<round_item> &);
	void flush (vote_solicitor_round const &) const;

private: // Dependencies
	nano::network & network;
	nano::rep_crawler & rep_crawler;
	nano::block_rebroadcaster & block_rebroadcaster;
	nano::network_constants const & network_constants;
	nano::stats & stats;
	nano::logger & logger;

private:
	// Solicitation state per election root, entries for dead elections are swept each round
	std::unordered_map<nano::qualified_root, entry> elections;
	std::size_t pending_count{ 0 };

	std::chrono::steady_clock::time_point last_round{};

	bool stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex{ mutex_identifier (mutexes::vote_solicitor) };
	std::thread thread;
};
}
