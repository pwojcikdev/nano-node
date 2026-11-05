#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/utility.hpp>
#include <nano/messages/fwd.hpp>
#include <nano/node/election_status.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/transport/transport.hpp>

namespace nano
{
class node_observers final
{
public:
	nano::observer_set<nano::cemented_block_info const &> block_cemented; // A block's cementing has committed; with a non-syncing store it may not be on disk yet
	nano::observer_set<bool> wallet;
	nano::observer_set<nano::vote_context const &, nano::vote_code> vote;
	nano::observer_set<nano::block_hash const &> active_started;
	nano::observer_set<nano::block_hash const &> active_stopped;
	nano::observer_set<nano::account const &, bool> account_balance;
	nano::observer_set<> disconnect;
	nano::observer_set<nano::root const &> work_cancel;
	nano::observer_set<nano::messages::telemetry_data const &, std::shared_ptr<nano::transport::channel> const &> telemetry;
	nano::observer_set<std::shared_ptr<nano::transport::tcp_socket>> socket_connected;
	nano::observer_set<std::shared_ptr<nano::transport::channel>> channel_connected;

	nano::container_info container_info () const;
};
}
