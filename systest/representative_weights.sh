#!/bin/bash
set -eu

source "$(dirname "$0")/lib/common.sh"

datadir=$(new_datadir)
for network in dev test; do
    status=0
    "$NANO_NODE_EXE" --network "$network" --data_path "$datadir" --compare_rep_weights > "$datadir/result" 2>&1 || status=$?
    test "$status" -eq 1
    grep -Fq "No bootstrap representative weights are available for the $network network" "$datadir/result"
done

for network in beta live; do
    ledger=$(new_datadir)
    "$NANO_NODE_EXE" --network "$network" --data_path "$ledger" --initialize
    "$NANO_NODE_EXE" --network "$network" --data_path "$ledger" --compare_rep_weights > "$ledger/comparison"
    grep -Eq '^ledger weight .+ at 1 blocks$' "$ledger/comparison"
done

# Persist a sample using the regular voting-weight sampler, then inspect it offline.
samples=$(new_datadir)
first_date=$(date -u +%F)
"$NANO_NODE_EXE" --daemon --network dev --data_path "$samples" \
    --config node.peering_port=0 'node.online_weight_minimum="0"' 'log.console.to_cerr=true' > "$samples/daemon.log" 2>&1 &
pid=$!
register_pid "$pid"
for ((i=0; i<120; ++i)); do
    if grep -q 'Updated trended weight' "$samples/daemon.log"; then break; fi
    kill -0 "$pid"
    sleep 0.25
done
grep -q 'Updated trended weight' "$samples/daemon.log"
interrupt_process "$pid"
wait "$pid"
"$NANO_NODE_EXE" --network dev --data_path "$samples" --debug_dump_trended_weight > "$samples/dump"
last_date=$(date -u +%F)
grep -Eq "^Timestamp ($first_date|$last_date) [0-9]{2}:[0-9]{2}:[0-9]{2} UTC Weight [0-9]+$" "$samples/dump"
