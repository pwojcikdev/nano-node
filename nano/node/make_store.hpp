#pragma once

#include <nano/node/fwd.hpp>
#include <nano/node/nodeconfig.hpp>

namespace nano
{
std::unique_ptr<nano::store::ledger_store> make_store (
nano::logger &,
nano::stats &,
std::filesystem::path const & path,
nano::ledger_constants & constants,
bool read_only = false,
bool add_db_postfix = true,
nano::node_config node_config = nano::node_config{});
}
