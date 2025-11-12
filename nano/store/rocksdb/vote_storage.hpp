#pragma once

#include <nano/store/vote_storage.hpp>

namespace nano::store::rocksdb
{
class component;
}

namespace nano::store::rocksdb
{
class vote_storage : public nano::store::vote_storage
{
private:
	nano::store::rocksdb::component & store;

public:
	explicit vote_storage (nano::store::rocksdb::component & store);
	std::size_t put (store::write_transaction const & transaction_a, std::shared_ptr<nano::vote> const & vote_a) override;
	std::deque<std::shared_ptr<nano::vote>> get (store::transaction const & transaction_a, nano::block_hash const & hash_a) override;
	size_t count (store::transaction const & transaction_a) const override;
	void clear (store::write_transaction const & transaction_a) override;
	iterator begin (store::transaction const & transaction_a, nano::vote_storage_key const & key_a) const override;
	iterator begin (store::transaction const & transaction_a) const override;
	iterator end (store::transaction const & transaction_a) const override;
};
} // namespace nano::store::rocksdb
