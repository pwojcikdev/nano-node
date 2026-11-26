#!/bin/bash
set -eux

# Extended ledger index CLI lifecycle on a fresh database:
# --database_info reports the persisted flags, --populate_extended_ledger_indices enables them,
# --drop_extended_ledger_indices disables them, and both commands are idempotent

source "$(dirname "$0")/lib/common.sh"

DATADIR=$(new_datadir)

$NANO_NODE_EXE --initialize --data_path "$DATADIR" --network dev

info() {
    $NANO_NODE_EXE --database_info --data_path "$DATADIR" --network dev
}

# A fresh database reports a numeric version with every extended index flag disabled
info | grep -Eq 'Version: +[0-9]+'
info | grep -Eq 'account_delegator_by_weight_index: +disabled'
info | grep -Eq 'account_receivable_by_amount_index: +disabled'
info | grep -Eq 'receive_block_by_send_block_index: +disabled'
info | grep -Eq 'account_block_by_height_index: +disabled'

# Populating marks every extended index flag enabled
$NANO_NODE_EXE --populate_extended_ledger_indices --data_path "$DATADIR" --network dev | grep -q 'Extended ledger indices populated'
info | grep -Eq 'account_delegator_by_weight_index: +enabled'
info | grep -Eq 'account_receivable_by_amount_index: +enabled'
info | grep -Eq 'receive_block_by_send_block_index: +enabled'
info | grep -Eq 'account_block_by_height_index: +enabled'

# A second populate is a no-op
$NANO_NODE_EXE --populate_extended_ledger_indices --data_path "$DATADIR" --network dev | grep -q 'already populated'

# Dropping disables every extended index flag
$NANO_NODE_EXE --drop_extended_ledger_indices --data_path "$DATADIR" --network dev | grep -q 'Extended ledger indices dropped'
info | grep -Eq 'account_delegator_by_weight_index: +disabled'
info | grep -Eq 'account_receivable_by_amount_index: +disabled'
info | grep -Eq 'receive_block_by_send_block_index: +disabled'
info | grep -Eq 'account_block_by_height_index: +disabled'

# A second drop is a no-op
$NANO_NODE_EXE --drop_extended_ledger_indices --data_path "$DATADIR" --network dev | grep -q 'not enabled'

# Pruned ledgers reject both populate commands and keep their persisted index flags.
DATADIR=$(new_datadir)
$NANO_NODE_EXE --initialize --data_path "$DATADIR" --network dev --enable_pruning --disable_topo_index
before=$(info)
for option in --populate_topo_index --populate_extended_ledger_indices; do
    status=0
    $NANO_NODE_EXE "$option" --data_path "$DATADIR" --network dev > "$DATADIR/populate-error" 2>&1 || status=$?
    test "$status" -eq 1
    grep -Fq 'cannot be populated on a pruned ledger' "$DATADIR/populate-error"
    test "$(info)" = "$before"
done

echo "All ledger_extended_index tests passed"
