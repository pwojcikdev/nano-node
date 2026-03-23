#include <nano/node/node.hpp>
#include <nano/node/rpc_api/command.hpp>
#include <nano/node/rpc_api/params.hpp>
#include <nano/node/rpc_api/registry.hpp>

using namespace nano::rpc_api;

// process: Process a block (async, complex)
// STUB - Very complex handler that requires block deserialization from JSON,
// state block subtype validation, block processing with result mapping,
// fork handling with force option, and async mode.
// Depends on block_impl() which deserializes blocks from property tree JSON.
// Left for incremental migration.
// REGISTER_RPC_COMMAND (process_block_cmd);
