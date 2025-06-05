#include <nano/secure/account_info.hpp>
#include <nano/secure/parallel_traversal.hpp>
#include <nano/store/db_val.hpp>
#include <nano/store/ledger/account.hpp>

namespace nano::store::ledger
{
account::account (store::backend & backend_a) :
	backend{ backend_a }
{
}

void account::put (store::write_transaction const & transaction, nano::account const & account, nano::account_info const & info)
{
	auto status = backend.put (transaction, tables::accounts, account, info);
	backend.release_assert_success (status);
}

bool account::get (store::transaction const & transaction, nano::account const & account, nano::account_info & info) const
{
	db_val value;
	auto status = backend.get (transaction, tables::accounts, account, value);
	release_assert (backend.success (status) || backend.not_found (status), backend.error_string (status));
	bool result = true;
	if (backend.success (status))
	{
		// TODO: WTF why reimplement deserialization?
		nano::bufferstream stream{ reinterpret_cast<uint8_t const *> (value.data ()), value.size () };
		result = info.deserialize (stream);
	}
	return result;
}

std::optional<nano::account_info> account::get (store::transaction const & transaction, nano::account const & account) const
{
	nano::account_info info;
	bool error = get (transaction, account, info);
	if (error)
	{
		return std::nullopt;
	}
	return info;
}

void account::del (store::write_transaction const & transaction, nano::account const & account)
{
	auto status = backend.del (transaction, tables::accounts, account);
	backend.release_assert_success (status);
}

bool account::exists (store::transaction const & transaction, nano::account const & account) const
{
	return backend.exists (transaction, tables::accounts, account);
}

size_t account::count (store::transaction const & transaction) const
{
	return backend.count (transaction, tables::accounts);
}

auto account::begin (store::transaction const & transaction, nano::account const & account) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::accounts, account) };
}

auto account::begin (store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::accounts) };
}

auto account::rbegin (store::transaction const & transaction) const -> reverse_iterator
{
	return reverse_iterator{ std::prev (end (transaction)) };
}

auto account::rend (store::transaction const & transaction) const -> reverse_iterator
{
	return reverse_iterator{ end (transaction) };
}

auto account::end (store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.end (transaction, tables::accounts) };
}

void account::for_each_par (std::function<void (store::read_transaction const &, iterator, iterator)> const & action) const
{
	parallel_traversal<nano::uint256_t> (
	[&action, this] (nano::uint256_t const & start, nano::uint256_t const & end, bool const is_last) {
		auto transaction = this->backend.tx_begin_read ();
		action (transaction, this->begin (transaction, start), !is_last ? this->begin (transaction, end) : this->end (transaction));
	});
}
}