#pragma once

#include <nano/lib/epoch.hpp>

#include <functional>

namespace std
{
template <>
struct hash<::nano::epoch>
{
	std::size_t operator() (::nano::epoch const & epoch_a) const
	{
		return std::underlying_type_t<::nano::epoch> (epoch_a);
	}
};
}
