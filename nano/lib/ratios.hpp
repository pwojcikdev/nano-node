#pragma once

#include <nano/lib/numbers.hpp>

#include <boost/multiprecision/cpp_int.hpp>

namespace nano
{
// SI dividers
extern nano::uint128_t const Knano_ratio; // 10^33 = 1000 nano
extern nano::uint128_t const nano_ratio; // 10^30 = 1 nano
extern nano::uint128_t const raw_ratio; // 10^0
}
