#include <nano/store/ledger/blocks_topo.hpp>

#include <nano/store/db_val_templ.hpp>

#include <boost/endian/conversion.hpp>

#include <cstring>

namespace nano::store::ledger
{
blocks_topo_view::blocks_topo_view (nano::store::backend & backend_a) :
	backend{ backend_a }
{
}

auto blocks_topo_view::make_key (uint64_t topo_height_a, nano::block_hash const & hash_a) -> key_type
{
	key_type result;
	result.clear ();

	auto topo_be = boost::endian::native_to_big (topo_height_a);
	static_assert (sizeof (topo_be) == 8);
	std::memcpy (result.bytes.data (), &topo_be, sizeof (topo_be));
	std::memcpy (result.bytes.data () + sizeof (topo_be), hash_a.bytes.data (), hash_a.bytes.size ());

	return result;
}

void blocks_topo_view::put (nano::store::write_transaction const & txn, uint64_t topo_height_a, nano::block_hash const & hash_a)
{
	auto const key = make_key (topo_height_a, hash_a);
	auto status = backend.put (txn, nano::store::table::blocks_topo, key, nullptr);
	backend.release_assert_success (status);
}

void blocks_topo_view::del (nano::store::write_transaction const & txn, uint64_t topo_height_a, nano::block_hash const & hash_a)
{
	auto const key = make_key (topo_height_a, hash_a);
	auto status = backend.del (txn, nano::store::table::blocks_topo, key);
	backend.release_assert_success (status);
}

auto blocks_topo_view::begin (nano::store::transaction const & txn) const -> iterator
{
	return iterator{ backend.begin (txn, nano::store::table::blocks_topo) };
}

auto blocks_topo_view::end (nano::store::transaction const & txn) const -> iterator
{
	return iterator{ backend.end (txn, nano::store::table::blocks_topo) };
}

uint64_t blocks_topo_view::topo_height (key_type const & key_a)
{
	uint64_t topo_be{ 0 };
	static_assert (sizeof (topo_be) == 8);
	std::memcpy (&topo_be, key_a.bytes.data (), sizeof (topo_be));
	return boost::endian::big_to_native (topo_be);
}

nano::block_hash blocks_topo_view::hash (key_type const & key_a)
{
	nano::block_hash result;
	std::memcpy (result.bytes.data (), key_a.bytes.data () + sizeof (uint64_t), result.bytes.size ());
	return result;
}
}
