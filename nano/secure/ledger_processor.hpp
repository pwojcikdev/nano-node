#pragma once

#include <nano/lib/block_type.hpp>
#include <nano/lib/blocks_raw.hpp>
#include <nano/lib/fwd.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/fwd.hpp>

namespace nano
{
class ledger_processor
{
public:
	ledger_processor (nano::secure::write_transaction const &, nano::ledger &);

	void process (nano::raw_block const & block);

	nano::block_status result{ nano::block_status::invalid };

private:
	void process_send (nano::raw_send_block const &);
	void process_receive (nano::raw_receive_block const &);
	void process_open (nano::raw_open_block const &);
	void process_change (nano::raw_change_block const &);
	void process_state (nano::raw_state_block const &);

	void state_block_impl (nano::raw_state_block const &);
	void epoch_block_impl (nano::raw_state_block const &);
	bool validate_epoch_block (nano::raw_state_block const &);
	bool valid_predecessor (nano::block_type current, nano::block_type predecessor) const;

	nano::secure::write_transaction const & transaction;
	nano::ledger & ledger;
};
}
