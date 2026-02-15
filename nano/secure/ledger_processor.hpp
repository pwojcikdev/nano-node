#pragma once

#include <nano/lib/blocks.hpp>
#include <nano/lib/fwd.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/fwd.hpp>

namespace nano
{
class ledger_processor final : public nano::mutable_block_visitor
{
public:
	ledger_processor (nano::secure::write_transaction const &, nano::ledger &);

	void send_block (nano::send_block &) override;
	void receive_block (nano::receive_block &) override;
	void open_block (nano::open_block &) override;
	void change_block (nano::change_block &) override;
	void state_block (nano::state_block &) override;

	nano::secure::write_transaction const & transaction;
	nano::ledger & ledger;
	nano::block_status result{ nano::block_status::invalid };

private:
	void state_block_impl (nano::state_block &);
	void epoch_block_impl (nano::state_block &);
	bool validate_epoch_block (nano::state_block const & block);
	bool valid_predecessor (nano::block const & block, nano::block const & previous) const;
};
}