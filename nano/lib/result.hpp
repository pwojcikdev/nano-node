#pragma once

#include <nano/boost/outcome.hpp>
#include <nano/lib/errors.hpp>

namespace nano
{
template <typename T>
using result = outcome::result<T, nano::error>;
}