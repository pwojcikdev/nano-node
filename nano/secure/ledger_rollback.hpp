#pragma once

#include <nano/lib/blocks.hpp>
#include <nano/lib/fwd.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/fwd.hpp>

#include <cstddef>
#include <deque>
#include <memory>

namespace nano
{
class ledger_rollback final : public nano::block_visitor
{
public:
	ledger_rollback (nano::secure::write_transaction &, nano::ledger &, std::deque<std::shared_ptr<nano::block>> & list, size_t depth, size_t max_depth);

	void send_block (nano::send_block const &) override;
	void receive_block (nano::receive_block const &) override;
	void open_block (nano::open_block const &) override;
	void change_block (nano::change_block const &) override;
	void state_block (nano::state_block const &) override;

	nano::secure::write_transaction & transaction;
	nano::ledger & ledger;
	std::deque<std::shared_ptr<nano::block>> & list;
	size_t const depth;
	size_t const max_depth;
	bool error{ false };
};
}