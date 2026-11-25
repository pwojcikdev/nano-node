#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/lib/common.sh"

if [ -z "${NANO_RPC_EXE-}" ] || [ ! -x "$NANO_RPC_EXE" ]; then
    echo "SKIP: NANO_RPC_EXE is not set or not executable"
    exit 0
fi
RPC_EXE="$(cd "$(dirname "$NANO_RPC_EXE")" && pwd)/$(basename "$NANO_RPC_EXE")"
DATADIR=$(new_datadir)
RUNTIME_INFO="$DATADIR/runtime_info.json"
RPC_PORT=$((20000 + RANDOM % 20000))
IPC_PORT=$((40000 + RANDOM % 20000))

cat > "$DATADIR/config-log.toml" <<'TOML'
[log]
flush_level = "debug"
[log.levels]
rpc_request = "debug"
TOML

request() {
    curl --noproxy '*' -gfs --max-time 2 -d "{\"action\":\"$1\"}" "http://[::1]:$RPC_PORT"
}

for control in false true; do
    cat > "$DATADIR/config-rpc.toml" <<TOML
enable_control = $control
port = $RPC_PORT
[process]
ipc_port = $IPC_PORT
TOML
    "$NANO_NODE_EXE" --daemon --network dev --data_path "$DATADIR" \
        --enable_rpc --runtime_info_file "$RUNTIME_INFO" \
        --config rpc.child_process.enable=true --config "rpc.child_process.rpc_path=$RPC_EXE" \
        --config node.ipc.tcp.enable=true --config "node.ipc.tcp.port=$IPC_PORT" \
        --config node.peering_port=0 &
    NODE_PID=$!
    register_pid "$NODE_PID"
    wait_for_file "$RUNTIME_INFO" "$NODE_PID"
    for ((i=0; i<60; i++)); do
        if request version | jq -e '.rpc_version' > /dev/null; then
            break
        fi
        sleep 0.5
    done
    if ((i == 60)); then
        echo "FAIL: RPC child did not serve a version request"
        exit 1
    fi
    response=$(request stop)
    if [ "$control" = false ]; then
        jq -e '.error == "RPC control is disabled"' <<< "$response" > /dev/null
        request version | jq -e '.rpc_version' > /dev/null
        interrupt_process "$NODE_PID"
    else
        jq -e '.success == ""' <<< "$response" > /dev/null
    fi

    for i in {1..60}; do
        kill -0 "$NODE_PID" 2>/dev/null || break
        sleep 0.5
    done
    if kill -0 "$NODE_PID" 2>/dev/null; then
        echo "FAIL: node still running after shutdown"
        exit 1
    fi
    wait "$NODE_PID"
    [ ! -e "$RUNTIME_INFO" ]
done

# The separate RPC process uses the same logging configuration as the node.
if ! grep -q '\[rpc_request\] \[debug\] Request .*version' "$DATADIR"/log/log_*.log; then
    echo "FAIL: the RPC child did not log the version request at the configured level"
    exit 1
fi
