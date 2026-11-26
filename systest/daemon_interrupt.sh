#!/bin/bash
set -eux

# Test that the daemon handles SIGINT gracefully

source "$(dirname "$0")/lib/common.sh"

DATADIR=$(new_datadir)
RUNTIME_INFO="$DATADIR/runtime_info.json"

# On Unix, hold startup in the configuration reader while a supervisor stops the
# daemon using its published PID. The same directory must be usable on restart.
if ! is_windows; then
    mkfifo "$DATADIR/config-node.toml"
    "$NANO_NODE_EXE" --daemon --network dev --data_path "$DATADIR" \
        --pid_file "$DATADIR/node.pid" --runtime_info_file "$RUNTIME_INFO" &
    starting_pid=$!
    register_pid "$starting_pid"
    (
        touch "$DATADIR/reading_config"
        while [ ! -f "$DATADIR/release_config" ]; do sleep 0.1; done
    ) > "$DATADIR/config-node.toml" &
    writer_pid=$!
    register_pid "$writer_pid"
    wait_for_file "$DATADIR/reading_config" "$starting_pid"
    test "$(cat "$DATADIR/node.pid")" = "$starting_pid"
    kill -TERM "$starting_pid"
    touch "$DATADIR/release_config"
    wait "$writer_pid"
    wait "$starting_pid"
    test ! -e "$DATADIR/node.pid"
    test ! -e "$RUNTIME_INFO"
    rm "$DATADIR/config-node.toml"
fi

# Start the node in daemon mode with ephemeral port
$NANO_NODE_EXE --daemon --network dev --data_path "$DATADIR" \
    --runtime_info_file "$RUNTIME_INFO" \
    --config node.peering_port=0 &
NODE_PID=$!
register_pid $NODE_PID

# Wait for runtime_info.json to appear (indicates node is ready)
wait_for_file "$RUNTIME_INFO" $NODE_PID

# Send an interrupt signal to the node process
interrupt_process $NODE_PID

# Check if the process has stopped using a timeout to avoid infinite waiting
if wait $NODE_PID; then
    echo "Node stopped successfully"
else
    echo "Node did not stop as expected"
    exit 1
fi
