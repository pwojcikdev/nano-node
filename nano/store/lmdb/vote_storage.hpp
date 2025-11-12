#pragma once

#include <nano/store/vote_storage.hpp>

#include <lmdb/libraries/liblmdb/lmdb.h>

namespace nano::store::lmdb
{
class component;
}

namespace nano::store::lmdb
{
class vote_storage : public nano::store::vote_storage
{
private:
	nano::store::lmdb::component & store;

public:
	explicit vote_storage (nano::store::lmdb::component & store);
	std::size_t put (store::write_transaction const & transaction_a, std::shared_ptr<nano::vote> const & vote_a) override;
	std::deque<std::shared_ptr<nano::vote>> get (store::transaction const & transaction_a, nano::block_hash const & hash_a) override;
	size_t count (store::transaction const & transaction_a) const override;
	void clear (store::write_transaction const & transaction_a) override;
	iterator begin (store::transaction const & transaction_a, nano::vote_storage_key const & key_a) const override;
	iterator begin (store::transaction const & transaction_a) const override;
	iterator end (store::transaction const & transaction_a) const override;

	/**
	 * Maps vote storage key (block_hash + account) to vote data
	 * nano::vote_storage_key -> vote (serialized)
	 */
	MDB_dbi vote_storage_handle{ 0 };
};
} // namespace nano::store::lmdb
