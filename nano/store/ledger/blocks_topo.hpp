#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

#include <cstdint>

namespace nano::store::ledger
{
class blocks_topo_view final
{
public:
	using key_type = nano::uint512_union;
	using iterator = store::typed_iterator<key_type, std::nullptr_t>;

public:
	explicit blocks_topo_view (nano::store::backend &);

	void put (nano::store::write_transaction const &, uint64_t topo_height, nano::block_hash const &);
	void del (nano::store::write_transaction const &, uint64_t topo_height, nano::block_hash const &);

	iterator begin (nano::store::transaction const &) const;
	iterator end (nano::store::transaction const &) const;

	static uint64_t topo_height (key_type const &);
	static nano::block_hash hash (key_type const &);

private:
	static key_type make_key (uint64_t topo_height, nano::block_hash const &);

private:
	nano::store::backend & backend;
};
}

