#pragma once

#include <nano/store/backend.hpp>

namespace nano::store::ledger
{
class version
{
public:
	explicit version (nano::store::backend &);

	void put (nano::store::write_transaction const &, uint64_t version);
	uint64_t get (nano::store::transaction const &) const;

private:
	nano::store::backend & backend;
};
}
