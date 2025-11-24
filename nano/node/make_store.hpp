#pragma once

#include <nano/lib/diagnosticsconfig.hpp>
#include <nano/lib/lmdbconfig.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/rocksdbconfig.hpp>
#include <nano/node/nodeconfig.hpp>

#include <chrono>

namespace nano
{
class ledger_constants;
class lmdb_config;
class rocksdb_config;
class stats;
class txn_tracking_config;
}

namespace nano::store
{
class ledger_store;
}

namespace nano
{
std::unique_ptr<nano::store::ledger_store> make_store (nano::logger &, nano::stats &, std::filesystem::path const & path, nano::ledger_constants & constants, bool read_only = false, bool add_db_postfix = true, nano::node_config node_config = nano::node_config{});
}
