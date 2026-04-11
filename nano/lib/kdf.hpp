#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>

#include <string>

namespace nano
{
class kdf final
{
public:
	kdf (unsigned const & kdf_work) :
		kdf_work{ kdf_work }
	{
	}

	void phs (nano::raw_key & result, std::string const & password, nano::uint256_union const & salt);

	nano::mutex mutex;
	unsigned const & kdf_work;
};
}
