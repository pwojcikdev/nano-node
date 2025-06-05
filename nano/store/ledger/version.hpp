#pragma once

#include <nano/store/backend.hpp>

namespace nano::store
{
class version
{
public:
	explicit version (store::backend &);

	void put (store::write_transaction const & tx, int version);
	int get (store::transaction const & tx) const;

private:
	store::backend & backend;
};
}
