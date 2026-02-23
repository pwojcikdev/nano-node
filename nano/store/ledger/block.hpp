#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/stored_block.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

#include <functional>
#include <optional>

namespace nano::store::ledger
{
class successor_view;

class block_view
{
public:
	using iterator = store::typed_iterator<nano::block_hash, nano::stored_block>;

public:
	block_view (nano::store::backend &, nano::store::ledger::successor_view &);

	void put (nano::store::write_transaction const &, nano::block_hash const &, nano::block const &);
	void put (nano::store::write_transaction const &, nano::block_hash const &, nano::raw_block const &, nano::block_sideband const &);
	void raw_put (nano::store::write_transaction const &, std::vector<uint8_t> const & data, nano::block_hash const &);
	std::optional<nano::stored_block> get (nano::store::transaction const &, nano::block_hash const &) const;
	void del (nano::store::write_transaction const &, nano::block_hash const &);
	bool exists (nano::store::transaction const &, nano::block_hash const &) const;
	uint64_t count (nano::store::transaction const &) const;
	iterator begin (nano::store::transaction const &, nano::block_hash const &) const;
	iterator begin (nano::store::transaction const &) const;
	iterator end (nano::store::transaction const &) const;
	void for_each_par (std::function<void (nano::store::read_transaction const &, iterator, iterator)> const & action) const;

private:
	void block_raw_get (nano::store::transaction const &, nano::block_hash const &, nano::store::db_val & value) const;

private:
	nano::store::backend & backend;
	nano::store::ledger::successor_view & successor_store;
};
}
