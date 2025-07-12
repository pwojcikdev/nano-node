#include <nano/secure/parallel_traversal.hpp>
#include <nano/store/ledger/pending.hpp>

namespace nano::store::ledger
{
pending::pending (store::backend & backend_a) :
	backend{ backend_a }
{
}

void pending::put (store::write_transaction const & transaction, nano::pending_key const & key, nano::pending_info const & pending)
{
	auto status = backend.put (transaction, tables::pending, key, pending);
	backend.release_assert_success (status);
}

void pending::del (store::write_transaction const & transaction, nano::pending_key const & key)
{
	auto status = backend.del (transaction, tables::pending, key);
	backend.release_assert_success (status);
}

auto pending::get (store::transaction const & transaction, nano::pending_key const & key) const -> std::optional<nano::pending_info>
{
	db_val value;
	auto status = backend.get (transaction, tables::pending, key, value);
	release_assert (backend.success (status) || backend.not_found (status), backend.error_string (status));
	std::optional<nano::pending_info> result;
	if (backend.success (status))
	{
		// TODO: WTF why reimplement deserialization?
		nano::bufferstream stream{ reinterpret_cast<uint8_t const *> (value.data ()), value.size () };
		result = nano::pending_info{};
		auto error = result.value ().deserialize (stream);
		release_assert (!error);
	}
	return result;
}

bool pending::exists (store::transaction const & transaction, nano::pending_key const & key) const
{
	return backend.exists (transaction, tables::pending, key);
}

bool pending::any (store::transaction const & transaction, nano::account const & account) const
{
	auto iterator = begin (transaction, nano::pending_key{ account, 0 });
	return iterator != end (transaction) && nano::pending_key (iterator->first).account == account;
}

auto pending::begin (store::transaction const & transaction, nano::pending_key const & key) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::pending, key) };
}

auto pending::begin (store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::pending) };
}

auto pending::end (store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.end (transaction, tables::pending) };
}

void pending::for_each_par (std::function<void (store::read_transaction const &, iterator, iterator)> const & action) const
{
	parallel_traversal<nano::uint512_t> (
	[&action, this] (nano::uint512_t const & start, nano::uint512_t const & end, bool const is_last) {
		nano::uint512_union union_start{ start };
		nano::uint512_union union_end{ end };
		nano::pending_key key_start{ union_start.uint256s[0].number (), union_start.uint256s[1].number () };
		nano::pending_key key_end{ union_end.uint256s[0].number (), union_end.uint256s[1].number () };
		auto transaction = this->backend.tx_begin_read ();
		action (transaction, this->begin (transaction, key_start), !is_last ? this->begin (transaction, key_end) : this->end (transaction));
	});
}
}