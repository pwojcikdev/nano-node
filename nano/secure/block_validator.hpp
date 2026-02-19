#pragma once

#include <nano/lib/block_sideband.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/secure/account_info.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/fwd.hpp>
#include <nano/secure/pending_info.hpp>

#include <memory>
#include <optional>
#include <variant>

namespace nano
{
class epochs;
class ledger;
class work_thresholds;

/**
 * Pre-fetched data needed for block validation.
 * All database lookups happen during construction (via create_validation_context).
 * The validator operates purely on this data — no I/O.
 */
struct block_validation_context
{
	nano::block & block;
	nano::block_hash hash;
	nano::account account;

	// Pre-fetched ledger state
	bool block_exists{ false };
	std::shared_ptr<nano::block> previous_block; // null if no previous
	std::optional<nano::account_info> old_account_info; // nullopt if account doesn't exist
	std::optional<nano::pending_info> pending_receive_info; // for receive/open blocks
	bool any_pending_exists{ false }; // whether any receivable exists for account
	bool source_block_exists{ false }; // whether the source/link block exists

	// For epoch blocks: balance of the previous block (0 if no previous)
	nano::amount prev_balance{ 0 };

	// Immutable config references
	nano::epochs const & epochs;
	nano::work_thresholds const & work;
	nano::account burn_account;
};

/**
 * Describes all mutations that should be applied to the ledger after a block passes validation.
 * Computed by the validator as a pure function over the validation context.
 * Applied by block_inserter, which is the only component that touches the database.
 */
struct block_insert_instructions
{
	nano::account account;
	nano::block_sideband sideband;
	nano::account_info new_account_info;

	// Pending entry to delete (receive consumes a pending send)
	std::optional<nano::pending_key> pending_to_delete;

	// Pending entry to create (send creates a pending receive)
	std::optional<std::pair<nano::pending_key, nano::pending_info>> pending_to_add;

	// Representative weight changes
	struct rep_weight_delta
	{
		nano::account old_rep;
		nano::amount old_balance;
		nano::account new_rep;
		nano::amount new_balance;
		bool is_new_account{ false };
	};
	rep_weight_delta rep_delta;

	// Stat to increment
	nano::stat::detail stat_detail{ nano::stat::detail::send };
};

/**
 * Creates a block_validation_context by performing all necessary database lookups.
 * This is the only function that touches the ledger/store for validation purposes.
 */
nano::block_validation_context create_validation_context (
nano::secure::write_transaction const & transaction,
nano::ledger & ledger,
nano::block & block);

/**
 * Pure validation logic for blocks. Operates entirely on a pre-fetched block_validation_context.
 * No database access, no side effects — all state is read from the context.
 *
 * On success, returns block_insert_instructions describing what mutations to apply.
 * On failure, returns a block_status error code.
 */
class block_validator
{
public:
	using result_type = std::variant<nano::block_insert_instructions, nano::block_status>;

	static result_type validate (nano::block_validation_context const & ctx);

	// Individual validation rules — public for unit testing
	static nano::block_status ensure_block_does_not_exist (nano::block_validation_context const & ctx);
	static nano::block_status ensure_valid_predecessor (nano::block_validation_context const & ctx);
	static nano::block_status ensure_valid_signature (nano::block_validation_context const & ctx);
	static nano::block_status ensure_not_burn_account (nano::block_validation_context const & ctx);
	static nano::block_status ensure_account_exists_for_non_open (nano::block_validation_context const & ctx);
	static nano::block_status ensure_no_double_account_open (nano::block_validation_context const & ctx);
	static nano::block_status ensure_previous_is_head (nano::block_validation_context const & ctx);
	static nano::block_status ensure_sufficient_work (nano::block_validation_context const & ctx, nano::block_details const & details);
	static nano::block_status ensure_no_negative_spend (nano::block_validation_context const & ctx);
	static nano::block_status ensure_source_exists (nano::block_validation_context const & ctx);
	static nano::block_status ensure_pending_receivable (nano::block_validation_context const & ctx);
	static nano::block_status ensure_receive_amount_correct (nano::block_validation_context const & ctx, nano::amount const & amount);
	static nano::block_status ensure_legacy_source_epoch_0 (nano::block_validation_context const & ctx);
	static nano::block_status ensure_open_has_source (nano::block_validation_context const & ctx);
	static nano::block_status ensure_state_balance_correct (nano::block_validation_context const & ctx, bool is_send, bool is_receive, nano::amount const & amount);

	// Epoch-specific validation
	static nano::block_status ensure_epoch_balance_unchanged (nano::block_validation_context const & ctx);
	static nano::block_status ensure_epoch_representative_unchanged (nano::block_validation_context const & ctx);
	static nano::block_status ensure_epoch_valid_upgrade (nano::block_validation_context const & ctx);
	static nano::block_status ensure_epoch_open_has_pending (nano::block_validation_context const & ctx);
	static nano::block_status ensure_epoch_open_has_zero_rep (nano::block_validation_context const & ctx);

private:
	static result_type validate_send_block (nano::block_validation_context const & ctx, nano::send_block const & block);
	static result_type validate_receive_block (nano::block_validation_context const & ctx, nano::receive_block const & block);
	static result_type validate_open_block (nano::block_validation_context const & ctx, nano::open_block const & block);
	static result_type validate_change_block (nano::block_validation_context const & ctx, nano::change_block const & block);
	static result_type validate_state_block (nano::block_validation_context const & ctx, nano::state_block const & block);
	static result_type validate_epoch_block (nano::block_validation_context const & ctx, nano::state_block const & block);

	// Helpers to build instructions
	static nano::block_insert_instructions build_send_instructions (nano::block_validation_context const & ctx, nano::send_block const & block);
	static nano::block_insert_instructions build_receive_instructions (nano::block_validation_context const & ctx, nano::receive_block const & block);
	static nano::block_insert_instructions build_open_instructions (nano::block_validation_context const & ctx, nano::open_block const & block);
	static nano::block_insert_instructions build_change_instructions (nano::block_validation_context const & ctx, nano::change_block const & block);
	static nano::block_insert_instructions build_state_instructions (nano::block_validation_context const & ctx, nano::state_block const & block, bool is_send, bool is_receive, nano::epoch epoch, nano::epoch source_epoch, nano::amount const & amount);
	static nano::block_insert_instructions build_epoch_instructions (nano::block_validation_context const & ctx, nano::state_block const & block, nano::epoch epoch);
};

/**
 * Applies block_insert_instructions to the ledger store.
 * This is the only component that performs mutations during block processing.
 */
class block_inserter
{
public:
	static void insert (
	nano::secure::write_transaction const & transaction,
	nano::ledger & ledger,
	nano::block & block,
	nano::block_insert_instructions const & instructions);
};
}
