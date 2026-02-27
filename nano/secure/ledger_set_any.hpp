#pragma once

#include <nano/lib/stored_block.hpp>
#include <nano/secure/account_iterator.hpp>
#include <nano/secure/fwd.hpp>
#include <nano/secure/receivable_iterator.hpp>

#include <optional>

namespace nano
{
class ledger_set_any
{
public:
	using account_iterator = nano::account_iterator<ledger_set_any>;
	using receivable_iterator = nano::receivable_iterator<ledger_set_any>;

	explicit ledger_set_any (nano::ledger const &);

public: // Operations on accounts
	std::optional<nano::amount> account_balance (nano::secure::transaction const &, nano::account const &) const;
	account_iterator account_begin (nano::secure::transaction const &) const;
	account_iterator account_end () const;
	std::optional<nano::account_info> account_get (nano::secure::transaction const &, nano::account const &) const;
	bool account_exists (nano::secure::transaction const &, nano::account const &) const;
	nano::block_hash account_head (nano::secure::transaction const &, nano::account const &) const;
	uint64_t account_height (nano::secure::transaction const &, nano::account const &) const;
	// Returns the next account entry equal or greater than 'account'
	// Mirrors std::map::lower_bound
	account_iterator account_lower_bound (nano::secure::transaction const &, nano::account const &) const;
	// Returns the next account entry greater than 'account'
	// Returns account_lower_bound (transaction, account + 1)
	// Mirrors std::map::upper_bound
	account_iterator account_upper_bound (nano::secure::transaction const &, nano::account const &) const;

public: // Operations on blocks
	std::optional<nano::account> block_account (nano::secure::transaction const &, nano::block_hash const &) const;
	std::optional<nano::amount> block_amount (nano::secure::transaction const &, nano::block_hash const &) const;
	std::optional<nano::amount> block_amount (nano::secure::transaction const &, nano::stored_block const &) const;
	std::optional<nano::amount> block_balance (nano::secure::transaction const &, nano::block_hash const &) const;
	bool block_exists (nano::secure::transaction const &, nano::block_hash const &) const;
	bool block_exists_or_pruned (nano::secure::transaction const &, nano::block_hash const &) const;
	std::optional<nano::stored_block> block_get (nano::secure::transaction const &, nano::block_hash const &) const;
	bool block_pruned (nano::secure::transaction const &, nano::block_hash const &) const;
	uint64_t block_height (nano::secure::transaction const &, nano::block_hash const &) const;
	std::optional<nano::block_hash> block_successor (nano::secure::transaction const &, nano::block_hash const &) const;
	std::optional<nano::block_hash> block_successor (nano::secure::transaction const &, nano::qualified_root const &) const;

public: // Operations on pending entries
	std::optional<nano::pending_info> pending_get (nano::secure::transaction const &, nano::pending_key const &) const;
	receivable_iterator receivable_end () const;
	bool receivable_exists (nano::secure::transaction const &, nano::account const &) const;
	// Returns the next receivable entry equal or greater than 'key'
	// Mirrors std::map::lower_bound
	std::optional<std::pair<nano::pending_key, nano::pending_info>> receivable_lower_bound (nano::secure::transaction const &, nano::account const &, nano::block_hash const &) const;
	// Returns the next receivable entry for an account greater than 'account'
	// Returns receivable_lower_bound (transaction, account + 1, 0)
	// Mirrors std::map::upper_bound
	receivable_iterator receivable_upper_bound (nano::secure::transaction const &, nano::account const &) const;
	// Returns the next receivable entry for the account 'account' with hash greater than 'hash'
	// Returns receivable_lower_bound (transaction, account + 1, hash)
	// Mirrors std::map::upper_bound
	receivable_iterator receivable_upper_bound (nano::secure::transaction const &, nano::account const &, nano::block_hash const &) const;

private:
	nano::ledger const & ledger;
};
}
