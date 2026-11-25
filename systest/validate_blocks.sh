#!/bin/bash
set -eu

source "$(dirname "$0")/lib/common.sh"
datadir=$(new_datadir)
runtime="$datadir/runtime.json"

"$NANO_NODE_EXE" --daemon --network dev --data_path "$datadir" \
    --enable_rpc --runtime_info_file "$runtime" \
    --config node.peering_port=0 --config node.enable_voting=false \
    --rpcconfig port=0 --rpcconfig enable_control=true > "$datadir/node.log" 2>&1 &
pid=$!
register_pid "$pid"
wait_for_file "$runtime" "$pid"
port=$(jq -er '.rpc_port' "$runtime")
rpc() {
    curl -gsSf --noproxy '*' --max-time 10 -d "$1" "http://[::1]:$port"
}

# Receive a transfer into an unopened account to create a state-open block on disk.
wallet=$(rpc '{"action":"wallet_create"}' | jq -er '.wallet')
dev_genesis_key=34F0A37AAD20F4A260F0A5B3CB3D7FB50673212263E58A380BC10474BB039CE4
genesis=$(rpc "{\"action\":\"wallet_add\",\"wallet\":\"$wallet\",\"key\":\"$dev_genesis_key\"}" | jq -er '.account')
recipient=$(rpc "{\"action\":\"account_create\",\"wallet\":\"$wallet\"}" | jq -er '.account')
send=$(rpc "{\"action\":\"send\",\"wallet\":\"$wallet\",\"source\":\"$genesis\",\"destination\":\"$recipient\",\"amount\":\"1\"}" | jq -er '.block')
rpc "{\"action\":\"receive\",\"wallet\":\"$wallet\",\"account\":\"$recipient\",\"block\":\"$send\"}" | jq -er '.block' > /dev/null

# A receive after an epoch upgrade retains the epoch of its earlier send.
send=$(rpc "{\"action\":\"send\",\"wallet\":\"$wallet\",\"source\":\"$genesis\",\"destination\":\"$genesis\",\"amount\":\"1\"}" | jq -er '.block')
rpc "{\"action\":\"epoch_upgrade\",\"epoch\":\"1\",\"key\":\"$dev_genesis_key\"}" | jq -e '.started == "1"' > /dev/null
for i in {1..60}; do
    version=$(rpc "{\"action\":\"account_info\",\"account\":\"$genesis\"}" | jq -er '.account_version')
    [ "$version" = 1 ] && break
    sleep 0.2
done
test "$version" = 1
rpc "{\"action\":\"receive\",\"wallet\":\"$wallet\",\"account\":\"$genesis\",\"block\":\"$send\"}" | jq -er '.block' > /dev/null
interrupt_process "$pid"
wait "$pid"

"$NANO_NODE_EXE" --network dev --data_path "$datadir" --validate_blocks > "$datadir/report"
grep -Fq 'Validation status: Ok' "$datadir/report"
