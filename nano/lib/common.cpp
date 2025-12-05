#include <nano/lib/assert.hpp>
#include <nano/lib/common.hpp>

#include <boost/algorithm/string/case_conv.hpp>

std::string nano::to_string (nano::database_backend const value)
{
	switch (value)
	{
		case nano::database_backend::lmdb:
			return "lmdb";
		case nano::database_backend::rocksdb:
			return "rocksdb";
	}
	release_assert (false);
}

std::optional<nano::database_backend> nano::parse_database_backend (std::string value)
{
	boost::algorithm::to_lower (value);

	if (value == "lmdb")
	{
		return nano::database_backend::lmdb;
	}
	if (value == "rocksdb")
	{
		return nano::database_backend::rocksdb;
	}
	return {};
}
