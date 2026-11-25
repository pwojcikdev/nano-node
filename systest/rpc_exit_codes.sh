#!/bin/bash
set -eu

source "$(dirname "$0")/lib/common.sh"

if [ -z "${NANO_RPC_EXE-}" ] || [ ! -x "$NANO_RPC_EXE" ]; then
    echo "SKIP: NANO_RPC_EXE is not set or not executable"
    exit 0
fi

"$NANO_RPC_EXE" --version > /dev/null
"$NANO_RPC_EXE" --help > /dev/null

DATADIR=$(new_datadir)
RUNTIME_INFO="$DATADIR/runtime_info.json"
"$NANO_NODE_EXE" --daemon --network dev --data_path "$DATADIR" \
    --config node.peering_port=0 --runtime_info_file "$RUNTIME_INFO" &
NODE_PID=$!
register_pid "$NODE_PID"
wait_for_file "$RUNTIME_INFO" "$NODE_PID"

# The node holds a port assigned by the OS.
PORT=$(jq -er '.peering_port' "$RUNTIME_INFO")
rpc=("$NANO_RPC_EXE" --daemon --network dev --data_path "$DATADIR" --config "port=$PORT")
status=0
if is_windows; then
    # Windows can allow a second listener to bind the port with SO_REUSEADDR.
    "${rpc[@]}" --config address=invalid || status=$?
else
    "${rpc[@]}" || status=$?
fi
if [ "$status" -ne 1 ]; then
    echo "FAIL: expected startup failure with status 1, got $status"
    exit 1
fi

interrupt_process "$NODE_PID"
wait "$NODE_PID"

# The same configuration now starts and a clean shutdown must report success.
"${rpc[@]}" &
RPC_PID=$!
register_pid "$RPC_PID"
for i in {1..60}; do
    if (: <>"/dev/tcp/::1/$PORT") 2>/dev/null; then
        interrupt_process "$RPC_PID"
        wait "$RPC_PID"
        exit 0
    fi
    kill -0 "$RPC_PID" 2>/dev/null || break
    sleep 0.5
done
echo "FAIL: standalone RPC did not listen on the released port"
exit 1
