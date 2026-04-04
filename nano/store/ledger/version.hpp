#pragma once

#include <nano/store/backend.hpp>

namespace nano::store::ledger
{
class version_view
{
public:
	explicit version_view (nano::store::backend &);

	void put (nano::store::write_transaction const &, uint64_t version);
	uint64_t get (nano::store::transaction const &) const;

	void put_topo_enabled (nano::store::write_transaction const &, bool enabled);
	bool get_topo_enabled (nano::store::transaction const &) const;

private:
	nano::store::backend & backend;
};
}
