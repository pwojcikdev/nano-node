#!/bin/bash
set -eu

source "$(dirname "$0")/lib/common.sh"
datadir=$(new_datadir)
runtime="$datadir/runtime.json"

"$NANO_NODE_EXE" --daemon --network dev --data_path "$datadir" \
    --enable_rpc --runtime_info_file "$runtime" \
    --config node.peering_port=0 --config node.enable_voting=true --disable_topo_index \
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
genesis_hash=$(rpc "{\"action\":\"account_info\",\"account\":\"$genesis\"}" | jq -er '.open_block')
genesis_json=$(rpc "{\"action\":\"block_info\",\"hash\":\"$genesis_hash\",\"json_block\":\"true\"}" | jq -cer '.contents')
recipient=$(rpc "{\"action\":\"account_create\",\"wallet\":\"$wallet\"}" | jq -er '.account')
send=$(rpc "{\"action\":\"send\",\"wallet\":\"$wallet\",\"source\":\"$genesis\",\"destination\":\"$recipient\",\"amount\":\"1\"}" | jq -er '.block')
recipient_open=$(rpc "{\"action\":\"receive\",\"wallet\":\"$wallet\",\"account\":\"$recipient\",\"block\":\"$send\"}" | jq -er '.block')

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
# Confirm the recipient's epoch block and the genesis chain before pruning offline.
for i in {1..60}; do
    version=$(rpc "{\"action\":\"account_info\",\"account\":\"$recipient\"}" | jq -er '.account_version')
    counts=$(rpc '{"action":"block_count"}')
    if [ "$version" = 1 ] && jq -e '.count == .cemented' <<< "$counts" > /dev/null; then break; fi
    sleep 0.2
done
test "$version" = 1
jq -e '.count == .cemented' <<< "$counts" > /dev/null
interrupt_process "$pid"
wait "$pid"

"$NANO_NODE_EXE" --network dev --data_path "$datadir" --validate_blocks > "$datadir/report"
grep -Fq 'Validation status: Ok' "$datadir/report"

# Check the same ledger against stricter test-network work thresholds.
status=0
NANO_TEST_GENESIS_BLOCK="$genesis_json" NANO_TEST_GENESIS_PUB="$(jq -r '.source' <<< "$genesis_json")" \
    NANO_TEST_EPOCH_1=ffffffffffffffff "$NANO_NODE_EXE" --network test --data_path "$datadir" \
    --validate_blocks > "$datadir/strict-report" 2>&1 || status=$?
grep -Fq "Invalid work for block $recipient_open" "$datadir/strict-report"
grep -Fq 'Validation status: Failed' "$datadir/strict-report"
test "$status" = 1

# The recipient's epoch block remains after pruning its nonzero-balance predecessor.
"$NANO_NODE_EXE" --network dev --data_path "$datadir" --debug_prune --enable_pruning \
    --config node.enable_voting=false --config node.experimental.max_pruning_depth=1
echo "{\"action\":\"pruned_exists\",\"hash\":\"$recipient_open\"}" | \
    "$NANO_NODE_EXE" --network dev --data_path "$datadir" --debug_rpc | jq -e '.exists == "1"' > /dev/null
"$NANO_NODE_EXE" --network dev --data_path "$datadir" --validate_blocks > "$datadir/pruned-report"
grep -Fq 'Validation status: Ok' "$datadir/pruned-report"

# Enabling pruning on a fresh ledger leaves genesis in the ordinary account chain.
pruning_dir=$(new_datadir)
"$NANO_NODE_EXE" --network dev --data_path "$pruning_dir" --initialize \
    --disable_topo_index --enable_pruning --config node.enable_voting=false
"$NANO_NODE_EXE" --network dev --data_path "$pruning_dir" --validate_blocks > "$pruning_dir/report"
grep -Fq 'Validation status: Ok' "$pruning_dir/report"
