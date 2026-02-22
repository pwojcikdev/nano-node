#pragma once

#include <nano/lib/stored_block.hpp>
#include <nano/secure/fwd.hpp>
#include <nano/secure/receivable_iterator.hpp>

#include <optional>

namespace nano
{
class ledger_set_cemented
{
public:
	using receivable_iterator = nano::receivable_iterator<ledger_set_cemented>;

	explicit ledger_set_cemented (nano::ledger const &);

public: // Operations on accounts
	std::optional<nano::amount> account_balance (nano::secure::transaction const &, nano::account const &) const;
	nano::block_hash account_head (nano::secure::transaction const &, nano::account const &) const;
	uint64_t account_height (nano::secure::transaction const &, nano::account const &) const;

public: // Operations on blocks
	std::optional<nano::amount> block_balance (nano::secure::transaction const &, nano::block_hash const &) const;
	bool block_exists (nano::secure::transaction const &, nano::block_hash const &) const;
	bool block_exists (nano::secure::transaction const &, nano::block const &) const;
	bool block_exists_or_pruned (nano::secure::transaction const &, nano::block_hash const &) const;
	std::optional<nano::stored_block> block_get (nano::secure::transaction const &, nano::block_hash const &) const;

public: // Operations on pending entries
	receivable_iterator receivable_end () const;
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
