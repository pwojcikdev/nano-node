#pragma once

namespace nano::store
{
enum class open_mode
{
	read_only,
	read_write
};

std::string_view to_string (open_mode);

using ledger_version = uint64_t;
}