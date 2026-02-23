#pragma once

#include <nano/lib/blocks_raw.hpp>
#include <nano/lib/fwd.hpp>
#include <nano/lib/stored_block.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/fwd.hpp>

#include <cstddef>
#include <deque>
#include <memory>

namespace nano
{
class ledger_rollback
{
public:
	ledger_rollback (nano::secure::write_transaction const &, nano::ledger &, std::deque<std::shared_ptr<nano::block>> & list, size_t depth, size_t max_depth);

	void rollback (nano::stored_block const & block);
	bool error{ false };

private:
	void rollback_send (nano::stored_block const &, nano::raw_send_block const &);
	void rollback_receive (nano::stored_block const &, nano::raw_receive_block const &);
	void rollback_open (nano::stored_block const &, nano::raw_open_block const &);
	void rollback_change (nano::stored_block const &, nano::raw_change_block const &);
	void rollback_state (nano::stored_block const &, nano::raw_state_block const &);

	nano::secure::write_transaction const & transaction;
	nano::ledger & ledger;
	std::deque<std::shared_ptr<nano::block>> & list;
	size_t const depth;
	size_t const max_depth;
};
}
