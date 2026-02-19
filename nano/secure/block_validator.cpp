#include <nano/lib/blocks.hpp>
#include <nano/lib/epochs.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/timer.hpp>
#include <nano/secure/block_validator.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/rep_weights.hpp>
#include <nano/store/ledger/account.hpp>
#include <nano/store/ledger/block.hpp>
#include <nano/store/ledger/pending.hpp>
#include <nano/store/ledger_store.hpp>

// ============================================================================
// Context factory
// ============================================================================

nano::block_validation_context nano::create_validation_context (
nano::secure::write_transaction const & transaction,
nano::ledger & ledger,
nano::block & block)
{
	auto hash = block.hash ();

	// Determine block existence
	bool block_exists = ledger.any.block_exists_or_pruned (transaction, hash);

	// Determine the account
	nano::account account;
	std::shared_ptr<nano::block> previous_block;
	nano::amount prev_balance{ 0 };

	auto previous_field = block.previous_field ();
	if (previous_field && !previous_field->is_zero ())
	{
		// Non-open block: get previous and derive account from it
		previous_block = ledger.store.block.get (transaction, previous_field.value ());
		if (previous_block)
		{
			account = previous_block->account ();
			auto bal = ledger.any.block_balance (transaction, previous_field.value ());
			if (bal)
			{
				prev_balance = bal.value ();
			}
		}
	}

	// For open/state blocks, account is in the block itself
	auto account_field = block.account_field ();
	if (account_field)
	{
		account = account_field.value ();
	}

	// Fetch current account info
	std::optional<nano::account_info> old_account_info;
	nano::account_info info;
	if (!ledger.store.account.get (transaction, account, info))
	{
		old_account_info = info;
	}

	// Fetch pending receive info (for receive/open blocks with a source/link)
	std::optional<nano::pending_info> pending_receive_info;

	// For legacy receive/open blocks: source field
	auto source_field = block.source_field ();
	if (source_field)
	{
		nano::pending_key key (account, source_field.value ());
		auto pending = ledger.store.pending.get (transaction, key);
		if (pending)
		{
			pending_receive_info = pending.value ();
		}
	}

	// For state blocks: link field (when it's a receive)
	auto link_field = block.link_field ();
	if (link_field && !pending_receive_info)
	{
		nano::pending_key key (account, link_field->as_block_hash ());
		auto pending = ledger.store.pending.get (transaction, key);
		if (pending)
		{
			pending_receive_info = pending.value ();
		}
	}

	// Check if any pending exists (for epoch open blocks)
	bool any_pending_exists = ledger.any.receivable_exists (transaction, account);

	// Check source/link block existence
	bool source_block_exists = false;
	if (source_field)
	{
		source_block_exists = ledger.any.block_exists_or_pruned (transaction, source_field.value ());
	}
	else if (link_field && !link_field->is_zero ())
	{
		source_block_exists = ledger.any.block_exists_or_pruned (transaction, link_field->as_block_hash ());
	}

	return nano::block_validation_context{
		block,
		hash,
		account,
		block_exists,
		previous_block,
		old_account_info,
		pending_receive_info,
		any_pending_exists,
		source_block_exists,
		prev_balance,
		ledger.constants.epochs,
		ledger.work,
		ledger.constants.burn_account,
	};
}

// ============================================================================
// Individual validation rules
// ============================================================================

nano::block_status nano::block_validator::ensure_block_does_not_exist (nano::block_validation_context const & ctx)
{
	return ctx.block_exists ? nano::block_status::old : nano::block_status::progress;
}

nano::block_status nano::block_validator::ensure_valid_predecessor (nano::block_validation_context const & ctx)
{
	if (!ctx.previous_block)
	{
		return nano::block_status::progress; // No previous block to check against (open blocks)
	}
	return ctx.block.valid_predecessor (*ctx.previous_block) ? nano::block_status::progress : nano::block_status::block_position;
}

nano::block_status nano::block_validator::ensure_valid_signature (nano::block_validation_context const & ctx)
{
	// For state/open blocks, the account is in the block itself
	// For legacy blocks with a previous, the account comes from the previous block
	return validate_message (ctx.account, ctx.hash, ctx.block.block_signature ())
	? nano::block_status::bad_signature
	: nano::block_status::progress;
}

nano::block_status nano::block_validator::ensure_not_burn_account (nano::block_validation_context const & ctx)
{
	return ctx.account == ctx.burn_account ? nano::block_status::opened_burn_account : nano::block_status::progress;
}

nano::block_status nano::block_validator::ensure_account_exists_for_non_open (nano::block_validation_context const & ctx)
{
	// For blocks that require a previous block (non-open blocks)
	return ctx.previous_block ? nano::block_status::progress : nano::block_status::gap_previous;
}

nano::block_status nano::block_validator::ensure_no_double_account_open (nano::block_validation_context const & ctx)
{
	// Account already has blocks — this is a fork if trying to open again
	return ctx.old_account_info.has_value () ? nano::block_status::fork : nano::block_status::progress;
}

nano::block_status nano::block_validator::ensure_previous_is_head (nano::block_validation_context const & ctx)
{
	if (!ctx.old_account_info)
	{
		return nano::block_status::progress;
	}
	auto const & info = ctx.old_account_info.value ();
	auto previous_field = ctx.block.previous_field ();
	if (!previous_field)
	{
		return nano::block_status::progress;
	}
	return info.head == previous_field.value () ? nano::block_status::progress : nano::block_status::fork;
}

nano::block_status nano::block_validator::ensure_sufficient_work (nano::block_validation_context const & ctx, nano::block_details const & details)
{
	auto difficulty = ctx.work.difficulty (ctx.block);
	auto threshold = ctx.work.threshold (ctx.block.work_version (), details);
	return difficulty >= threshold ? nano::block_status::progress : nano::block_status::insufficient_work;
}

nano::block_status nano::block_validator::ensure_no_negative_spend (nano::block_validation_context const & ctx)
{
	if (!ctx.old_account_info)
	{
		return nano::block_status::progress;
	}
	auto send_block_ptr = dynamic_cast<nano::send_block const *> (&ctx.block);
	if (send_block_ptr)
	{
		return ctx.old_account_info->balance.number () >= send_block_ptr->hashables.balance.number ()
		? nano::block_status::progress
		: nano::block_status::negative_spend;
	}
	return nano::block_status::progress;
}

nano::block_status nano::block_validator::ensure_source_exists (nano::block_validation_context const & ctx)
{
	return ctx.source_block_exists ? nano::block_status::progress : nano::block_status::gap_source;
}

nano::block_status nano::block_validator::ensure_pending_receivable (nano::block_validation_context const & ctx)
{
	return ctx.pending_receive_info.has_value () ? nano::block_status::progress : nano::block_status::unreceivable;
}

nano::block_status nano::block_validator::ensure_receive_amount_correct (nano::block_validation_context const & ctx, nano::amount const & expected_amount)
{
	if (!ctx.pending_receive_info)
	{
		return nano::block_status::unreceivable;
	}
	return ctx.pending_receive_info->amount == expected_amount ? nano::block_status::progress : nano::block_status::balance_mismatch;
}

nano::block_status nano::block_validator::ensure_legacy_source_epoch_0 (nano::block_validation_context const & ctx)
{
	if (!ctx.pending_receive_info)
	{
		return nano::block_status::unreceivable;
	}
	return ctx.pending_receive_info->epoch == nano::epoch::epoch_0 ? nano::block_status::progress : nano::block_status::unreceivable;
}

nano::block_status nano::block_validator::ensure_open_has_source (nano::block_validation_context const & ctx)
{
	auto source_field = ctx.block.source_field ();
	if (source_field && !source_field->is_zero ())
	{
		return nano::block_status::progress;
	}
	return nano::block_status::gap_source;
}

nano::block_status nano::block_validator::ensure_state_balance_correct (nano::block_validation_context const & ctx, bool is_send, bool is_receive, nano::amount const & amount)
{
	if (!is_send && !is_receive)
	{
		// Change-only: balance must not change, and link must be zero
		auto state = dynamic_cast<nano::state_block const *> (&ctx.block);
		if (state && !state->hashables.link.is_zero ())
		{
			return nano::block_status::progress; // Has link, will be checked as receive
		}
		return amount.is_zero () ? nano::block_status::progress : nano::block_status::balance_mismatch;
	}
	return nano::block_status::progress;
}

nano::block_status nano::block_validator::ensure_epoch_balance_unchanged (nano::block_validation_context const & ctx)
{
	auto state = dynamic_cast<nano::state_block const *> (&ctx.block);
	if (!state)
	{
		return nano::block_status::progress;
	}
	auto old_balance = ctx.old_account_info ? ctx.old_account_info->balance : nano::amount{ 0 };
	return state->hashables.balance == old_balance ? nano::block_status::progress : nano::block_status::balance_mismatch;
}

nano::block_status nano::block_validator::ensure_epoch_representative_unchanged (nano::block_validation_context const & ctx)
{
	auto state = dynamic_cast<nano::state_block const *> (&ctx.block);
	if (!state)
	{
		return nano::block_status::progress;
	}
	if (ctx.old_account_info)
	{
		return state->hashables.representative == ctx.old_account_info->representative
		? nano::block_status::progress
		: nano::block_status::representative_mismatch;
	}
	// New account: representative must be zero (burn account)
	return state->hashables.representative.is_zero ()
	? nano::block_status::progress
	: nano::block_status::representative_mismatch;
}

nano::block_status nano::block_validator::ensure_epoch_valid_upgrade (nano::block_validation_context const & ctx)
{
	auto state = dynamic_cast<nano::state_block const *> (&ctx.block);
	if (!state)
	{
		return nano::block_status::progress;
	}
	auto epoch = ctx.epochs.epoch (state->hashables.link);
	if (ctx.old_account_info)
	{
		return nano::epochs::is_sequential (ctx.old_account_info->epoch (), epoch)
		? nano::block_status::progress
		: nano::block_status::block_position;
	}
	// Unopened account: epoch must be > 0
	return static_cast<std::underlying_type_t<nano::epoch>> (epoch) > 0
	? nano::block_status::progress
	: nano::block_status::block_position;
}

nano::block_status nano::block_validator::ensure_epoch_open_has_pending (nano::block_validation_context const & ctx)
{
	return ctx.any_pending_exists ? nano::block_status::progress : nano::block_status::gap_epoch_open_pending;
}

nano::block_status nano::block_validator::ensure_epoch_open_has_zero_rep (nano::block_validation_context const & ctx)
{
	auto state = dynamic_cast<nano::state_block const *> (&ctx.block);
	if (!state)
	{
		return nano::block_status::progress;
	}
	return state->hashables.representative.is_zero ()
	? nano::block_status::progress
	: nano::block_status::representative_mismatch;
}

// ============================================================================
// Instruction builders
// ============================================================================

nano::block_insert_instructions nano::block_validator::build_send_instructions (nano::block_validation_context const & ctx, nano::send_block const & block)
{
	auto const & info = ctx.old_account_info.value ();
	auto amount = info.balance.number () - block.hashables.balance.number ();

	nano::block_details details (nano::epoch::epoch_0, false, false, false);
	nano::block_sideband sideband (ctx.account, 0, block.hashables.balance, info.block_count + 1, nano::seconds_since_epoch (), details, nano::epoch::epoch_0);
	nano::account_info new_info (ctx.hash, info.representative, info.open_block, block.hashables.balance, nano::seconds_since_epoch (), info.block_count + 1, nano::epoch::epoch_0);

	nano::block_insert_instructions instructions;
	instructions.account = ctx.account;
	instructions.sideband = sideband;
	instructions.new_account_info = new_info;
	instructions.pending_to_add = std::make_pair (
	nano::pending_key (block.hashables.destination, ctx.hash),
	nano::pending_info (ctx.account, amount, nano::epoch::epoch_0));
	instructions.rep_delta = { info.representative, info.balance, info.representative, block.hashables.balance, false };
	instructions.stat_detail = nano::stat::detail::send;
	return instructions;
}

nano::block_insert_instructions nano::block_validator::build_receive_instructions (nano::block_validation_context const & ctx, nano::receive_block const & block)
{
	auto const & info = ctx.old_account_info.value ();
	auto const & pending = ctx.pending_receive_info.value ();
	auto new_balance = info.balance.number () + pending.amount.number ();

	nano::block_details details (nano::epoch::epoch_0, false, false, false);
	nano::block_sideband sideband (ctx.account, 0, new_balance, info.block_count + 1, nano::seconds_since_epoch (), details, nano::epoch::epoch_0);
	nano::account_info new_info (ctx.hash, info.representative, info.open_block, new_balance, nano::seconds_since_epoch (), info.block_count + 1, nano::epoch::epoch_0);

	nano::block_insert_instructions instructions;
	instructions.account = ctx.account;
	instructions.sideband = sideband;
	instructions.new_account_info = new_info;
	instructions.pending_to_delete = nano::pending_key (ctx.account, block.hashables.source);
	instructions.rep_delta = { info.representative, info.balance, info.representative, nano::amount (new_balance), false };
	instructions.stat_detail = nano::stat::detail::receive;
	return instructions;
}

nano::block_insert_instructions nano::block_validator::build_open_instructions (nano::block_validation_context const & ctx, nano::open_block const & block)
{
	auto const & pending = ctx.pending_receive_info.value ();

	nano::block_details details (nano::epoch::epoch_0, false, false, false);
	nano::block_sideband sideband (block.hashables.account, 0, pending.amount, 1, nano::seconds_since_epoch (), details, nano::epoch::epoch_0);
	nano::account_info new_info (ctx.hash, block.hashables.representative, ctx.hash, pending.amount.number (), nano::seconds_since_epoch (), 1, nano::epoch::epoch_0);

	nano::block_insert_instructions instructions;
	instructions.account = block.hashables.account;
	instructions.sideband = sideband;
	instructions.new_account_info = new_info;
	instructions.pending_to_delete = nano::pending_key (block.hashables.account, block.hashables.source);
	instructions.rep_delta = { nano::account{}, nano::amount{ 0 }, block.hashables.representative, pending.amount, true };
	instructions.stat_detail = nano::stat::detail::open;
	return instructions;
}

nano::block_insert_instructions nano::block_validator::build_change_instructions (nano::block_validation_context const & ctx, nano::change_block const & block)
{
	auto const & info = ctx.old_account_info.value ();

	nano::block_details details (nano::epoch::epoch_0, false, false, false);
	nano::block_sideband sideband (ctx.account, 0, info.balance, info.block_count + 1, nano::seconds_since_epoch (), details, nano::epoch::epoch_0);
	nano::account_info new_info (ctx.hash, block.hashables.representative, info.open_block, info.balance, nano::seconds_since_epoch (), info.block_count + 1, nano::epoch::epoch_0);

	nano::block_insert_instructions instructions;
	instructions.account = ctx.account;
	instructions.sideband = sideband;
	instructions.new_account_info = new_info;
	instructions.rep_delta = { info.representative, info.balance, block.hashables.representative, info.balance, false };
	instructions.stat_detail = nano::stat::detail::change;
	return instructions;
}

nano::block_insert_instructions nano::block_validator::build_state_instructions (nano::block_validation_context const & ctx, nano::state_block const & block, bool is_send, bool is_receive, nano::epoch epoch, nano::epoch source_epoch, nano::amount const & amount)
{
	nano::account_info info;
	if (ctx.old_account_info)
	{
		info = ctx.old_account_info.value ();
	}

	nano::block_details details (epoch, is_send, is_receive, false);
	nano::block_sideband sideband (block.hashables.account, 0, 0, info.block_count + 1, nano::seconds_since_epoch (), details, source_epoch);
	nano::account_info new_info (ctx.hash, block.hashables.representative, info.open_block.is_zero () ? ctx.hash : info.open_block, block.hashables.balance, nano::seconds_since_epoch (), info.block_count + 1, epoch);

	nano::block_insert_instructions instructions;
	instructions.account = block.hashables.account;
	instructions.sideband = sideband;
	instructions.new_account_info = new_info;

	if (is_send)
	{
		instructions.pending_to_add = std::make_pair (
		nano::pending_key (block.hashables.link.as_account (), ctx.hash),
		nano::pending_info (block.hashables.account, amount.number (), epoch));
	}
	else if (!block.hashables.link.is_zero ())
	{
		instructions.pending_to_delete = nano::pending_key (block.hashables.account, block.hashables.link.as_block_hash ());
	}

	instructions.rep_delta = { info.representative, info.balance, block.hashables.representative, block.hashables.balance, info.head.is_zero () };
	instructions.stat_detail = nano::stat::detail::state_block;
	return instructions;
}

nano::block_insert_instructions nano::block_validator::build_epoch_instructions (nano::block_validation_context const & ctx, nano::state_block const & block, nano::epoch epoch)
{
	nano::account_info info;
	if (ctx.old_account_info)
	{
		info = ctx.old_account_info.value ();
	}

	nano::block_details details (epoch, false, false, true);
	nano::block_sideband sideband (block.hashables.account, 0, 0, info.block_count + 1, nano::seconds_since_epoch (), details, nano::epoch::epoch_0);
	nano::account_info new_info (ctx.hash, block.hashables.representative, info.open_block.is_zero () ? ctx.hash : info.open_block, info.balance, nano::seconds_since_epoch (), info.block_count + 1, epoch);

	nano::block_insert_instructions instructions;
	instructions.account = block.hashables.account;
	instructions.sideband = sideband;
	instructions.new_account_info = new_info;
	// Epoch blocks don't change rep weights or pending entries
	instructions.rep_delta = { info.representative, info.balance, info.representative, info.balance, false };
	instructions.stat_detail = nano::stat::detail::epoch_block;
	return instructions;
}

// ============================================================================
// Per-block-type validation
// ============================================================================

nano::block_validator::result_type nano::block_validator::validate_send_block (nano::block_validation_context const & ctx, nano::send_block const & block)
{
	nano::block_status s;

	if (s = ensure_block_does_not_exist (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_account_exists_for_non_open (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_valid_predecessor (ctx); s != nano::block_status::progress) return s;

	// Verify the previous block is the account head (fork check)
	{
		auto const & info = ctx.old_account_info.value ();
		if (info.head != block.hashables.previous)
			return nano::block_status::fork;
	}

	if (s = ensure_valid_signature (ctx); s != nano::block_status::progress) return s;

	nano::block_details details (nano::epoch::epoch_0, false, false, false);
	if (s = ensure_sufficient_work (ctx, details); s != nano::block_status::progress) return s;

	// Negative spend check
	{
		auto const & info = ctx.old_account_info.value ();
		if (info.balance.number () < block.hashables.balance.number ())
			return nano::block_status::negative_spend;
	}

	return build_send_instructions (ctx, block);
}

nano::block_validator::result_type nano::block_validator::validate_receive_block (nano::block_validation_context const & ctx, nano::receive_block const & block)
{
	nano::block_status s;

	if (s = ensure_block_does_not_exist (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_account_exists_for_non_open (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_valid_predecessor (ctx); s != nano::block_status::progress) return s;

	{
		auto const & info = ctx.old_account_info.value ();
		if (info.head != block.hashables.previous)
			return nano::block_status::fork;
	}

	if (s = ensure_valid_signature (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_source_exists (ctx); s != nano::block_status::progress) return s;

	// Head check (same as fork check for receive)
	{
		auto const & info = ctx.old_account_info.value ();
		if (info.head != block.hashables.previous)
			return nano::block_status::gap_previous;
	}

	if (s = ensure_pending_receivable (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_legacy_source_epoch_0 (ctx); s != nano::block_status::progress) return s;

	nano::block_details details (nano::epoch::epoch_0, false, false, false);
	if (s = ensure_sufficient_work (ctx, details); s != nano::block_status::progress) return s;

	return build_receive_instructions (ctx, block);
}

nano::block_validator::result_type nano::block_validator::validate_open_block (nano::block_validation_context const & ctx, nano::open_block const & block)
{
	nano::block_status s;

	if (s = ensure_block_does_not_exist (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_valid_signature (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_source_exists (ctx); s != nano::block_status::progress) return s;

	// For open_block, account_get returning error means account doesn't exist (good)
	// account_get returning success means account already opened (fork)
	if (ctx.old_account_info.has_value ())
		return nano::block_status::fork;

	if (s = ensure_pending_receivable (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_not_burn_account (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_legacy_source_epoch_0 (ctx); s != nano::block_status::progress) return s;

	nano::block_details details (nano::epoch::epoch_0, false, false, false);
	if (s = ensure_sufficient_work (ctx, details); s != nano::block_status::progress) return s;

	return build_open_instructions (ctx, block);
}

nano::block_validator::result_type nano::block_validator::validate_change_block (nano::block_validation_context const & ctx, nano::change_block const & block)
{
	nano::block_status s;

	if (s = ensure_block_does_not_exist (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_account_exists_for_non_open (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_valid_predecessor (ctx); s != nano::block_status::progress) return s;

	{
		auto const & info = ctx.old_account_info.value ();
		if (info.head != block.hashables.previous)
			return nano::block_status::fork;
	}

	if (s = ensure_valid_signature (ctx); s != nano::block_status::progress) return s;

	nano::block_details details (nano::epoch::epoch_0, false, false, false);
	if (s = ensure_sufficient_work (ctx, details); s != nano::block_status::progress) return s;

	return build_change_instructions (ctx, block);
}

nano::block_validator::result_type nano::block_validator::validate_state_block (nano::block_validation_context const & ctx, nano::state_block const & block)
{
	nano::block_status s;

	if (s = ensure_block_does_not_exist (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_valid_signature (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_not_burn_account (ctx); s != nano::block_status::progress) return s;

	auto epoch = nano::epoch::epoch_0;
	auto source_epoch = nano::epoch::epoch_0;
	nano::amount amount (block.hashables.balance);
	bool is_send = false;
	bool is_receive = false;

	if (ctx.old_account_info)
	{
		// Account already exists
		auto const & info = ctx.old_account_info.value ();
		epoch = info.epoch ();

		if (block.hashables.previous.is_zero ())
			return nano::block_status::fork; // Trying to "open" an existing account

		if (!ctx.previous_block)
			return nano::block_status::gap_previous; // Previous block doesn't exist

		is_send = block.hashables.balance < info.balance;
		is_receive = !is_send && !block.hashables.link.is_zero ();
		amount = is_send ? nano::amount (info.balance.number () - amount.number ()) : nano::amount (amount.number () - info.balance.number ());

		if (block.hashables.previous != info.head)
			return nano::block_status::fork;
	}
	else
	{
		// Account does not exist
		if (!block.previous ().is_zero ())
			return nano::block_status::gap_previous;

		is_receive = true;
		if (block.hashables.link.is_zero ())
			return nano::block_status::gap_source;
	}

	// Receive validation
	if (!is_send)
	{
		if (!block.hashables.link.is_zero ())
		{
			if (!ctx.source_block_exists)
				return nano::block_status::gap_source;

			if (!ctx.pending_receive_info)
				return nano::block_status::unreceivable;

			if (amount != ctx.pending_receive_info->amount)
				return nano::block_status::balance_mismatch;

			source_epoch = ctx.pending_receive_info->epoch;
			epoch = std::max (epoch, source_epoch);
		}
		else
		{
			// No link: change-only, balance must stay the same
			if (!amount.is_zero ())
				return nano::block_status::balance_mismatch;
		}
	}

	nano::block_details details (epoch, is_send, is_receive, false);
	if (s = ensure_sufficient_work (ctx, details); s != nano::block_status::progress) return s;

	return build_state_instructions (ctx, block, is_send, is_receive, epoch, source_epoch, amount);
}

nano::block_validator::result_type nano::block_validator::validate_epoch_block (nano::block_validation_context const & ctx, nano::state_block const & block)
{
	nano::block_status s;

	if (s = ensure_block_does_not_exist (ctx); s != nano::block_status::progress) return s;

	// Epoch blocks are signed by the epoch signer, not the account
	auto epoch_signer = ctx.epochs.signer (ctx.epochs.epoch (block.hashables.link));
	if (validate_message (epoch_signer, ctx.hash, block.signature))
		return nano::block_status::bad_signature;

	if (s = ensure_not_burn_account (ctx); s != nano::block_status::progress) return s;

	if (ctx.old_account_info)
	{
		auto const & info = ctx.old_account_info.value ();

		if (block.hashables.previous.is_zero ())
			return nano::block_status::fork;

		if (block.hashables.previous != info.head)
			return nano::block_status::fork;

		if (s = ensure_epoch_representative_unchanged (ctx); s != nano::block_status::progress) return s;
	}
	else
	{
		// Epoch open for non-existing account
		if (s = ensure_epoch_open_has_zero_rep (ctx); s != nano::block_status::progress) return s;
		if (s = ensure_epoch_open_has_pending (ctx); s != nano::block_status::progress) return s;
	}

	if (s = ensure_epoch_valid_upgrade (ctx); s != nano::block_status::progress) return s;
	if (s = ensure_epoch_balance_unchanged (ctx); s != nano::block_status::progress) return s;

	auto epoch = ctx.epochs.epoch (block.hashables.link);
	nano::block_details details (epoch, false, false, true);
	if (s = ensure_sufficient_work (ctx, details); s != nano::block_status::progress) return s;

	return build_epoch_instructions (ctx, block, epoch);
}

// ============================================================================
// Main dispatch
// ============================================================================

nano::block_validator::result_type nano::block_validator::validate (nano::block_validation_context const & ctx)
{
	switch (ctx.block.type ())
	{
		case nano::block_type::send:
			return validate_send_block (ctx, static_cast<nano::send_block const &> (ctx.block));
		case nano::block_type::receive:
			return validate_receive_block (ctx, static_cast<nano::receive_block const &> (ctx.block));
		case nano::block_type::open:
			return validate_open_block (ctx, static_cast<nano::open_block const &> (ctx.block));
		case nano::block_type::change:
			return validate_change_block (ctx, static_cast<nano::change_block const &> (ctx.block));
		case nano::block_type::state:
		{
			auto const & state = static_cast<nano::state_block const &> (ctx.block);
			// Check if this is an epoch block candidate
			if (ctx.epochs.is_epoch_link (state.hashables.link))
			{
				// Replicate the original validate_epoch_block pre-check logic:
				// If previous exists, and balance equals previous balance, it's an epoch block.
				// If previous doesn't exist and block is not signed by account, check epoch signer.
				bool is_epoch = false;
				if (!state.hashables.previous.is_zero ())
				{
					if (ctx.previous_block)
					{
						is_epoch = (state.hashables.balance == ctx.prev_balance);
					}
					else
					{
						// Previous doesn't exist — check signatures to disambiguate
						bool account_signed = !validate_message (state.hashables.account, ctx.hash, state.signature);
						if (!account_signed)
						{
							auto epoch_signer = ctx.epochs.signer (ctx.epochs.epoch (state.hashables.link));
							bool epoch_signed = !validate_message (epoch_signer, ctx.hash, state.signature);
							if (!epoch_signed)
							{
								return nano::block_status::bad_signature;
							}
							// It has an epoch link and is epoch-signed but previous is missing
							// The original code returns gap_previous from validate_epoch_block
							// then falls through to epoch_block_impl which returns old/gap_previous
							return nano::block_status::gap_previous;
						}
						// Account-signed with epoch link = regular state send
						is_epoch = false;
					}
				}
				else
				{
					// No previous — epoch open block (balance should be zero for new account)
					is_epoch = (state.hashables.balance == nano::amount{ 0 });
				}

				if (is_epoch)
				{
					return validate_epoch_block (ctx, state);
				}
			}
			return validate_state_block (ctx, state);
		}
		default:
			return nano::block_status::invalid;
	}
}

// ============================================================================
// Block inserter
// ============================================================================

void nano::block_inserter::insert (
nano::secure::write_transaction const & transaction,
nano::ledger & ledger,
nano::block & block,
nano::block_insert_instructions const & instructions)
{
	auto hash = block.hash ();

	// Set sideband and store the block
	block.sideband_set (instructions.sideband);
	ledger.store.block.put (transaction, hash, block);

	// Update account info
	nano::account_info old_info;
	if (instructions.rep_delta.is_new_account)
	{
		// Pass default (empty) old_info for new accounts
	}
	else
	{
		// For existing accounts, we need the old info for update_account's logic
		ledger.store.account.get (transaction, instructions.account, old_info);
	}
	ledger.update_account (transaction, instructions.account, old_info, instructions.new_account_info);

	// Delete consumed pending entry
	if (instructions.pending_to_delete)
	{
		ledger.store.pending.del (transaction, instructions.pending_to_delete.value ());
	}

	// Create new pending entry
	if (instructions.pending_to_add)
	{
		auto const & [key, info] = instructions.pending_to_add.value ();
		ledger.store.pending.put (transaction, key, info);
	}

	// Update representative weights
	auto const & rd = instructions.rep_delta;
	if (rd.is_new_account)
	{
		ledger.rep_weights.add (transaction, rd.new_rep, rd.new_balance);
	}
	else if (rd.old_rep == rd.new_rep)
	{
		// Same representative, just adjust the balance delta
		if (rd.new_balance > rd.old_balance)
		{
			ledger.rep_weights.add (transaction, rd.new_rep, rd.new_balance.number () - rd.old_balance.number ());
		}
		else if (rd.new_balance < rd.old_balance)
		{
			ledger.rep_weights.sub (transaction, rd.old_rep, rd.old_balance.number () - rd.new_balance.number ());
		}
		// Equal balances: no weight change (e.g., epoch blocks)
	}
	else
	{
		// Different representative: move weight
		ledger.rep_weights.move_add_sub (transaction, rd.old_rep, rd.old_balance, rd.new_rep, rd.new_balance);
	}

	// Record stat
	ledger.stats.inc (nano::stat::type::ledger, instructions.stat_detail);
}
