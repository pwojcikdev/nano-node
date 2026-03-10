#pragma once

#include <nano/node/bootstrap/bootstrap_context.hpp>
#include <nano/node/fwd.hpp>

#include <memory>

namespace nano
{
class bootstrap_service
{
public:
	bootstrap_service (nano::node_config const &, nano::ledger &, nano::ledger_notifications &, nano::block_processor &, nano::network &, nano::stats &, nano::logger &);
	~bootstrap_service ();

	void start ();
	void stop ();

	/**
	 * Process bootstrap messages coming from the network
	 */
	void process (nano::messages::asc_pull_ack const & message, std::shared_ptr<nano::transport::channel> const &);

	/**
	 * Clears priority and blocking accounts state
	 */
	void reset ();

	/**
	 * Adds an account to the priority set
	 */
	void prioritize (nano::account const & account);

	std::size_t blocked_size () const;
	std::size_t priority_size () const;
	std::size_t score_size () const;

	bool prioritized (nano::account const &) const;
	bool blocked (nano::account const &) const;

	nano::bootstrap::account_sets_index::info_t info () const;

	struct status_result
	{
		size_t priorities;
		size_t blocking;
	};
	status_result status () const;

	nano::container_info container_info () const;

private:
	std::unique_ptr<nano::bootstrap::bootstrap_context> ctx;
	nano::ledger_notifications & ledger_notifications;
};
}
