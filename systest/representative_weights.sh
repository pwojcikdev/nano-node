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
