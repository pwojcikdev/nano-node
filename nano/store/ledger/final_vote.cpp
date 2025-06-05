#include <nano/secure/parallel_traversal.hpp>
#include <nano/store/ledger/final_vote.hpp>

namespace nano::store::ledger
{
final_vote::final_vote (store::backend & backend_a) :
	backend{ backend_a }
{
}

bool final_vote::put (store::write_transaction const & transaction, nano::qualified_root const & root, nano::block_hash const & hash)
{
	db_val value;
	auto status = backend.get (transaction, tables::final_votes, root, value);
	release_assert (backend.success (status) || backend.not_found (status), backend.error_string (status));
	bool result = true;
	if (backend.success (status))
	{
		result = static_cast<nano::block_hash> (value) == hash;
	}
	else
	{
		status = backend.put (transaction, tables::final_votes, root, hash);
		backend.release_assert_success (status);
	}
	return result;
}

std::optional<nano::block_hash> final_vote::get (store::transaction const & transaction, nano::qualified_root const & qualified_root) const
{
	db_val result;
	auto status = backend.get (transaction, tables::final_votes, qualified_root, result);
	std::optional<nano::block_hash> final_vote_hash;
	if (backend.success (status))
	{
		final_vote_hash = static_cast<nano::block_hash> (result);
	}
	return final_vote_hash;
}

void final_vote::del (store::write_transaction const & transaction, nano::qualified_root const & root)
{
	auto status = backend.del (transaction, tables::final_votes, root);
	backend.release_assert_success (status);
}

size_t final_vote::count (store::transaction const & transaction) const
{
	return backend.count (transaction, tables::final_votes);
}

bool final_vote::empty (store::transaction const & transaction) const
{
	return backend.empty (transaction, tables::final_votes);
}

void final_vote::clear (store::write_transaction const & transaction)
{
	auto status = backend.clear (transaction, nano::tables::final_votes);
	backend.release_assert_success (status);
}

auto final_vote::begin (store::transaction const & transaction, nano::qualified_root const & root) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::final_votes, root) };
}

auto final_vote::begin (store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::final_votes) };
}

auto final_vote::end (store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.end (transaction, tables::final_votes) };
}

void final_vote::for_each_par (std::function<void (store::read_transaction const &, iterator, iterator)> const & action) const
{
	parallel_traversal<nano::uint512_t> (
	[&action, this] (nano::uint512_t const & start, nano::uint512_t const & end, bool const is_last) {
		auto transaction = this->backend.tx_begin_read ();
		action (transaction, this->begin (transaction, start), !is_last ? this->begin (transaction, end) : this->end (transaction));
	});
}
}