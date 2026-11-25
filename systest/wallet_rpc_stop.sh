#!/bin/bash
set -eu
source "$(dirname "$0")/lib/common.sh"

if [ -z "${NANO_WALLET_EXE-}" ] || [ ! -x "$NANO_WALLET_EXE" ]; then
    echo "SKIP: NANO_WALLET_EXE is not set or not executable"
    exit 0
fi

DATADIR=$(new_datadir)
OUTPUT="$DATADIR/wallet.out"
: > "$OUTPUT"
QT_QPA_PLATFORM=offscreen "$NANO_WALLET_EXE" --network dev --data_path "$DATADIR" \
    --enable_rpc --config node.peering_port=0 --rpcconfig port=0 --rpcconfig enable_control=true \
    > "$OUTPUT" 2>&1 &
WALLET_PID=$!
register_pid "$WALLET_PID"

# The wallet logs the port assigned by the OS when its in-process listener opens.
PORT=""
for i in {1..60}; do
    PORT=$(sed -n 's/.*RPC listening address: \[::1\]:\([0-9][0-9]*\).*/\1/p' "$OUTPUT")
    [ -n "$PORT" ] && break
    kill -0 "$WALLET_PID" 2>/dev/null || break
    sleep 0.5
done
[ -n "$PORT" ] || { echo "FAIL: the wallet RPC did not start"; exit 1; }
REPLY=$(curl -gsSf --noproxy '*' --max-time 5 -d '{"action":"stop"}' "http://[::1]:$PORT")
jq -e '.success == ""' <<< "$REPLY" >/dev/null

for i in {1..40}; do
    if ! kill -0 "$WALLET_PID" 2>/dev/null; then
        wait "$WALLET_PID"
        exit 0
    fi
    sleep 0.5
done
echo "FAIL: the wallet acknowledged stop and continued running"
exit 1
