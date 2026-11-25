#pragma once

#include <string_view>

namespace nano::store
{
enum class open_mode
{
	read_only,
	read_write,
	create,
	read_only_backup, // Read existing data for a physical backup, including committed WAL records
};

std::string_view to_string (open_mode);

using ledger_version = uint64_t;
}