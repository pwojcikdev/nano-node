#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/db_val.hpp>

namespace nano::wallet
{
class wallet_value
{
public:
	wallet_value () = default;
	wallet_value (store::db_val const &);
	wallet_value (nano::raw_key const &, uint64_t);
	store::db_val val () const;
	nano::raw_key key;
	uint64_t work;
};
}
