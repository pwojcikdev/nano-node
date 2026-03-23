#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>

using namespace nano::rpc_api;

// account_history: Get transaction history for an account (sync, complex)
// STUB - Complex handler that depends on the legacy history_visitor class
// which uses the block visitor pattern to categorize transactions as
// send/receive/change etc. This visitor is tightly coupled to
// boost::property_tree and the json_handler class.
// Left for incremental migration when history_visitor is decoupled.
// REGISTER_RPC_COMMAND (account_history_cmd);
