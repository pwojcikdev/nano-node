#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/voting/vote_factory.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace nano
{
class vote_broadcaster final
{
public:
	vote_broadcaster (nano::vote_factory &, nano::vote_processor &, nano::local_vote_history &, nano::vote_spacing &, nano::network &, nano::stats &, std::shared_ptr<nano::transport::channel> inproc_channel);

	/**
	 * Signs verified candidates via vote_factory, records in local_vote_history,
	 * flags vote_spacing, and invokes action callback for each generated vote.
	 */
	void vote (std::vector<nano::vote_factory::verified_candidate> const &, std::function<void (std::shared_ptr<nano::vote> const &)> const & action);

	/**
	 * Floods a vote to the network via vote_processor and network flooding.
	 */
	void broadcast (std::shared_ptr<nano::vote> const &, nano::stat::type stat_type) const;

	/**
	 * Collects up to confirm_ack_hashes_max candidates from a deque,
	 * filtering via spacing.votable() and deduplicating roots.
	 * Mutates the candidates deque by popping consumed entries.
	 */
	std::vector<nano::vote_factory::verified_candidate> collect_votable_batch (std::deque<nano::vote_factory::verified_candidate> & candidates, nano::stat::type stat_type);

private:
	nano::vote_factory & factory;
	nano::vote_processor & vote_processor;
	nano::local_vote_history & history;
	nano::vote_spacing & spacing;
	nano::network & network;
	nano::stats & stats;
	std::shared_ptr<nano::transport::channel> inproc_channel;
};
}
