#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/node/fwd.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace nano
{
class vote_broadcaster final
{
public:
	vote_broadcaster (nano::wallets &, nano::vote_processor &, nano::local_vote_history &, nano::vote_spacing &, nano::network &, nano::stats &, std::shared_ptr<nano::transport::channel> inproc_channel);

	/**
	 * Creates votes for all local representatives, records them in local_vote_history,
	 * flags vote_spacing, and invokes the action callback for each generated vote.
	 */
	void vote (std::vector<nano::block_hash> const & hashes, std::vector<nano::root> const & roots, bool is_final, std::function<void (std::shared_ptr<nano::vote> const &)> const & action);

	/**
	 * Floods a vote to the network via vote_processor and network flooding.
	 */
	void broadcast (std::shared_ptr<nano::vote> const &, nano::stat::type stat_type) const;

	using candidate_t = std::pair<nano::root, nano::block_hash>;

	struct batch_result
	{
		std::vector<nano::block_hash> hashes;
		std::vector<nano::root> roots;
	};

	/**
	 * Collects up to confirm_ack_hashes_max candidates from a deque,
	 * filtering via spacing.votable() and deduplicating roots.
	 * Mutates the candidates deque by popping consumed entries.
	 */
	batch_result collect_votable_batch (std::deque<candidate_t> & candidates, nano::stat::type stat_type);

private:
	nano::wallets & wallets;
	nano::vote_processor & vote_processor;
	nano::local_vote_history & history;
	nano::vote_spacing & spacing;
	nano::network & network;
	nano::stats & stats;
	std::shared_ptr<nano::transport::channel> inproc_channel;
};
}
