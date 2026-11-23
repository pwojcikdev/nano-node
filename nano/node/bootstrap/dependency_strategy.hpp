#pragma once

#include <nano/node/bootstrap/bootstrap_context.hpp>
#include <nano/node/bootstrap/lookup_index.hpp>

#include <thread>

namespace nano::bootstrap
{
class dependency_strategy
{
public:
	explicit dependency_strategy (bootstrap_context & ctx);

	void start ();
	void stop ();
	void run_one ();

	// Looks up the account of a block known to exist but missing from the ledger, so that the priority strategy pulls the account
	void lookup (nano::block_hash const &);

	bool process (nano::messages::asc_pull_ack::account_info_payload const & response, async_tag const & tag);

	// A request of this strategy got no answer in time
	void timeout (async_tag const & tag);

	// A request of this strategy got an answer of the wrong type
	void failure (async_tag const & tag);

	void reset ();

	nano::container_info container_info () const;

private:
	// Why a lookup came back without an account
	enum class lookup_failure
	{
		empty, // The peer does not know the block
		timeout, // No answer in time
		invalid, // An answer of the wrong type
	};

	void run ();
	void run_sync ();

	// Loop of the thread that looks up the accounts of tracked blocks
	void run_lookups ();

	// Waits for the block due for a lookup the longest and sends its lookup to a peer that has not failed on it
	void run_lookup_one ();

	nano::block_hash next_blocking ();
	nano::block_hash wait_blocking ();
	bool request_info (nano::block_hash hash, std::shared_ptr<nano::transport::channel> const & channel);

	// Whether the strategy runs, so that tracked blocks get looked up
	bool enabled () const;

	// Whether the block is in the ledger or was pruned from it, so that looking it up is pointless
	bool in_ledger (nano::block_hash const &) const;

	// Schedules another lookup of a tracked block whose lookup failed, or forgets the block once its retries run out
	void lookup_failed (async_tag const & tag, lookup_failure);

	bootstrap_context & ctx;
	nano::bootstrap::lookup_index lookups; // Missing blocks whose account is being looked up
	std::thread thread;
	std::thread sync_thread;
	std::thread lookup_thread;
};
}
