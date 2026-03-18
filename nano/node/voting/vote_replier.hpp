#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/voting/vote_broadcaster.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace nano
{
class vote_replier final
{
public:
	vote_replier (nano::ledger &, nano::wallets &, nano::vote_processor &, nano::local_vote_history &, nano::network &, nano::network_params &, nano::stats &, nano::logger &, std::shared_ptr<nano::transport::channel> inproc_channel);
	~vote_replier ();

	using request_type = std::vector<std::pair<nano::block_hash, nano::root>>;

	/**
	 * Process vote requests: look up blocks, classify as final/normal, create and send votes to channel.
	 * Called synchronously from request_aggregator threads.
	 * @return number of hashes that were replied to
	 */
	std::size_t reply (nano::secure::transaction const &, request_type const &, std::shared_ptr<nano::transport::channel> const &);

	nano::container_info container_info () const;

private:
	nano::ledger & ledger;
	nano::network_params & network_params;
	nano::stats & stats;
	nano::logger & logger;
	// spacing_impl must be declared before broadcaster (initialization order)
	std::unique_ptr<nano::vote_spacing> spacing_impl;
	nano::vote_spacing & spacing;
	nano::vote_broadcaster broadcaster;
};
}
