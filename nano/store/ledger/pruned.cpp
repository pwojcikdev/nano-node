#include <nano/crypto_lib/random_pool.hpp>
#include <nano/secure/parallel_traversal.hpp>
#include <nano/store/ledger/pruned.hpp>

namespace nano::store::ledger
{
pruned::pruned (nano::store::backend & backend_a) :
	backend{ backend_a }
{
}

void pruned::put (nano::store::write_transaction const & transaction, nano::block_hash const & hash)
{
	auto status = backend.put (transaction, tables::pruned, hash, nullptr);
	backend.release_assert_success (status);
}

void pruned::del (nano::store::write_transaction const & transaction, nano::block_hash const & hash)
{
	auto status = backend.del (transaction, tables::pruned, hash);
	backend.release_assert_success (status);
}

bool pruned::exists (nano::store::transaction const & transaction, nano::block_hash const & hash) const
{
	return backend.exists (transaction, tables::pruned, hash);
}

nano::block_hash pruned::random (nano::store::transaction const & transaction) const
{
	nano::block_hash random_hash;
	nano::random_pool::generate_block (random_hash.bytes.data (), random_hash.bytes.size ());
	auto existing = begin (transaction, random_hash);
	if (existing == end (transaction))
	{
		existing = begin (transaction);
	}
	return existing != end (transaction) ? existing->first : 0;
}

size_t pruned::count (nano::store::transaction const & transaction) const
{
	return backend.count (transaction, tables::pruned);
}

void pruned::clear (nano::store::write_transaction const & transaction)
{
	auto status = backend.clear (transaction, tables::pruned);
	backend.release_assert_success (status);
}

auto pruned::begin (nano::store::transaction const & transaction, nano::block_hash const & hash) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::pruned, hash) };
}

auto pruned::begin (nano::store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::pruned) };
}

auto pruned::end (nano::store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.end (transaction, tables::pruned) };
}

void pruned::for_each_par (std::function<void (nano::store::read_transaction const &, iterator, iterator)> const & action) const
{
	parallel_traversal<nano::uint256_t> (
	[&action, this] (nano::uint256_t const & start, nano::uint256_t const & end, bool const is_last) {
		auto transaction = this->backend.tx_begin_read ();
		action (transaction, this->begin (transaction, start), !is_last ? this->begin (transaction, end) : this->end (transaction));
	});
}
}