#pragma once

#include <nano/store/fwd.hpp>

namespace nano::store
{
class meta_view
{
public:
	explicit meta_view (nano::store::backend &);

	void put_version (nano::store::write_transaction const &, uint64_t version);
	uint64_t get_version (nano::store::transaction const &) const;
	bool version_exists (nano::store::transaction const &) const;

private:
	nano::store::backend & backend;

private:
	static uint64_t constexpr version_key{ 1 };
};
}