#!/bin/bash
set -eux

source "$(dirname "$0")/lib/common.sh"

DATADIR=$(new_datadir)
RUNTIME_INFO="$DATADIR/runtime_info.json"
NANO_NODE_EXE="$(cd "$(dirname "$NANO_NODE_EXE")" && pwd)/$(basename "$NANO_NODE_EXE")"

# A packaged service runs from its data directory, away from the installed schema.
(
    cd "$DATADIR"
    exec "$NANO_NODE_EXE" --daemon --network dev --data_path "$DATADIR" \
        --enable_rpc --runtime_info_file "$RUNTIME_INFO" \
        --config node.peering_port=0 --rpcconfig port=0
) &
NODE_PID=$!
register_pid "$NODE_PID"

wait_for_file "$RUNTIME_INFO" "$NODE_PID"
PORT=$(jq -er '.rpc_port' "$RUNTIME_INFO")
RESPONSE=$(curl --noproxy '*' -gfs --max-time 5 -d '{}' "[::1]:$PORT/api/v2/IsAlive")
jq -e '.message_type == "IsAlive"' <<< "$RESPONSE"
