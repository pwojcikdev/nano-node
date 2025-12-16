#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/block_w_sideband.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

#include <functional>
#include <optional>

namespace nano::store::ledger
{
class block_view
{
public:
	using iterator = store::typed_iterator<nano::block_hash, block_w_sideband>;

public:
	explicit block_view (nano::store::backend &);

	void put (nano::store::write_transaction const &, nano::block_hash const &, nano::block const &);
	void raw_put (nano::store::write_transaction const &, std::vector<uint8_t> const & data, nano::block_hash const &);
	std::optional<nano::block_hash> successor (nano::store::transaction const &, nano::block_hash const &) const;
	void successor_clear (nano::store::write_transaction const &, nano::block_hash const &);
	std::shared_ptr<nano::block> get (nano::store::transaction const &, nano::block_hash const &) const;
	void del (nano::store::write_transaction const &, nano::block_hash const &);
	bool exists (nano::store::transaction const &, nano::block_hash const &) const;
	uint64_t count (nano::store::transaction const &) const;
	iterator begin (nano::store::transaction const &, nano::block_hash const &) const;
	iterator begin (nano::store::transaction const &) const;
	iterator end (nano::store::transaction const &) const;
	void for_each_par (std::function<void (nano::store::read_transaction const &, iterator, iterator)> const & action) const;

private:
	void block_raw_get (nano::store::transaction const &, nano::block_hash const &, nano::store::db_val & value) const;
	size_t block_successor_offset (size_t, nano::block_type) const;
	nano::block_type block_type_from_raw (void const * data) const; // TODO: Use span

private:
	nano::store::backend & backend;
};
}
