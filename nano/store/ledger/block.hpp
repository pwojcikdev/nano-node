#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/block_w_sideband.hpp>
#include <nano/store/typed_iterator.hpp>

#include <functional>
#include <optional>

namespace nano::store::ledger
{
class block
{
public:
	using iterator = store::typed_iterator<nano::block_hash, block_w_sideband>;

public:
	explicit block (store::backend &);

	void put (store::write_transaction const & tx, nano::block_hash const & hash, nano::block const & block);
	void raw_put (store::write_transaction const & tx, std::vector<uint8_t> const & data, nano::block_hash const & hash);
	std::optional<nano::block_hash> successor (store::transaction const & tx, nano::block_hash const & hash) const;
	void successor_clear (store::write_transaction const & tx, nano::block_hash const & hash);
	std::shared_ptr<nano::block> get (store::transaction const & tx, nano::block_hash const & hash) const;
	void del (store::write_transaction const & tx, nano::block_hash const & hash);
	bool exists (store::transaction const & tx, nano::block_hash const & hash) const;
	uint64_t count (store::transaction const & tx) const;
	iterator begin (store::transaction const & tx, nano::block_hash const & hash) const;
	iterator begin (store::transaction const & tx) const;
	iterator end (store::transaction const & tx) const;
	void for_each_par (std::function<void (store::read_transaction const & tx, iterator, iterator)> const & action) const;

private:
	void block_raw_get (store::transaction const & tx, nano::block_hash const & hash, db_val & value) const;
	size_t block_successor_offset (size_t size, nano::block_type type) const;
	nano::block_type block_type_from_raw (void const * data) const; // TODO: Use span

private:
	store::backend & backend;
};
}
