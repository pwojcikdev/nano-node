#pragma once

#include <nano/lib/enum_flags.hpp>

#include <magic_enum.hpp>

#include <ostream>
#include <sstream>
#include <string>

namespace nano
{
template <typename E>
std::ostream & operator<< (std::ostream & os, enum_flags<E> const & flags)
{
	using underlying_t = typename enum_flags<E>::underlying_t;
	bool first = true;
	for (auto val : magic_enum::enum_values<E> ())
	{
		if (static_cast<underlying_t> (val) == 0)
		{
			continue;
		}
		if (flags.test (val))
		{
			if (!first)
			{
				os << ", ";
			}
			os << magic_enum::enum_name (val);
			first = false;
		}
	}
	if (first)
	{
		os << "none";
	}
	return os;
}

template <typename E>
std::string to_string (enum_flags<E> const & flags)
{
	std::ostringstream ss;
	ss << flags;
	return ss.str ();
}
}
