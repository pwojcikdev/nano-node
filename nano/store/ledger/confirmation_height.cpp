#include <nano/secure/parallel_traversal.hpp>
#include <nano/store/ledger/confirmation_height.hpp>

namespace nano::store
{
confirmation_height::confirmation_height (store::backend & backend_a) :
	backend{ backend_a }
{
}

void confirmation_height::put (store::write_transaction const & transaction, nano::account const & account, nano::confirmation_height_info const & confirmation_height_info)
{
	auto status = backend.put (transaction, tables::confirmation_height, account, confirmation_height_info);
	backend.release_assert_success (status);
}

bool confirmation_height::get (store::transaction const & transaction, nano::account const & account, nano::confirmation_height_info & confirmation_height_info) const
{
	db_val value;
	auto status = backend.get (transaction, tables::confirmation_height, account, value);
	release_assert (backend.success (status) || backend.not_found (status), backend.error_string (status));
	bool result = true;
	if (backend.success (status))
	{
		// TODO: WTF why reimplement deserialization?
		nano::bufferstream stream{ reinterpret_cast<uint8_t const *> (value.data ()), value.size () };
		result = confirmation_height_info.deserialize (stream);
	}
	if (result)
	{
		confirmation_height_info.height = 0;
		confirmation_height_info.frontier = 0;
	}
	return result;
}

std::optional<nano::confirmation_height_info> confirmation_height::get (store::transaction const & transaction, nano::account const & account)
{
	nano::confirmation_height_info info;
	bool error = get (transaction, account, info);
	if (error)
	{
		return std::nullopt;
	}
	return info;
}

bool confirmation_height::exists (store::transaction const & transaction, nano::account const & account) const
{
	return backend.exists (transaction, tables::confirmation_height, account);
}

void confirmation_height::del (store::write_transaction const & transaction, nano::account const & account)
{
	auto status = backend.del (transaction, tables::confirmation_height, account);
	backend.release_assert_success (status);
}

uint64_t confirmation_height::count (store::transaction const & transaction) const
{
	return backend.count (transaction, tables::confirmation_height);
}

void confirmation_height::clear (store::write_transaction const & transaction, nano::account const & account)
{
	del (transaction, account);
}

void confirmation_height::clear (store::write_transaction const & transaction)
{
	backend.drop (transaction, nano::tables::confirmation_height);
}

auto confirmation_height::begin (store::transaction const & transaction, nano::account const & account) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::confirmation_height, account) };
}

auto confirmation_height::begin (store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::confirmation_height) };
}

auto confirmation_height::end (store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.end (transaction, tables::confirmation_height) };
}

void confirmation_height::for_each_par (std::function<void (store::read_transaction const &, iterator, iterator)> const & action) const
{
	parallel_traversal<nano::uint256_t> (
	[&action, this] (nano::uint256_t const & start, nano::uint256_t const & end, bool const is_last) {
		auto transaction = this->backend.tx_begin_read ();
		action (transaction, this->begin (transaction, start), !is_last ? this->begin (transaction, end) : this->end (transaction));
	});
}
}