#!/bin/bash
set -eu

source "$(dirname "$0")/lib/common.sh"
datadir=$(new_datadir)

node() {
    "$NANO_NODE_EXE" --data_path "$datadir" --network dev "$@"
}

# Leave a real LMDB ledger in place, as on a node that has switched backends.
printf '[node]\ndatabase_backend = "lmdb"\n' > "$datadir/config-node.toml"
node --initialize
node --snapshot
test -f "$datadir/snapshot.ldb"
node --vacuum
test -f "$datadir/backup.vacuum.ldb"
node --debug_block_count | grep -q 'Block count: 1'
cp "$datadir/data.ldb" "$datadir/reference.ldb"

# Retry interrupted LMDB vacuum before and after replacement of the live pathname.
staging="$datadir/.lmdb-vacuum"
printf 'unfinished' > "$staging/vacuumed.ldb"
printf 'unfinished' > "$staging/original.ldb"
node --vacuum
test ! -e "$staging/vacuumed.ldb"
test ! -e "$staging/original.ldb"
cp "$datadir/backup.vacuum.ldb" "$staging/original.ldb"
node --vacuum
test ! -e "$staging/original.ldb"
node --debug_block_count | grep -q 'Block count: 1'

# A rejected migration setting must leave the destination available for a corrected retry.
if node --migrate_database_lmdb_to_rocksdb --config node.rocksdb.write_cache=0 > "$datadir/migration-error" 2>&1; then
    exit 1
fi
grep -q 'write_cache' "$datadir/migration-error"
test ! -e "$datadir/rocksdb"
node --migrate_database_lmdb_to_rocksdb --config node.rocksdb.write_cache=4
test -f "$datadir/rocksdb/CURRENT"

printf '[node]\ndatabase_backend = "rocksdb"\n' > "$datadir/config-node.toml"
node --initialize
# The first RocksDB vacuum must create its nested destination even without a prior snapshot.
node --vacuum
node --debug_block_count | grep -q 'Block count: 1'
node --snapshot
test -f "$datadir/rocksdb/backup/CURRENT"

snapshot_dir=$(new_datadir)
cp "$datadir/config-node.toml" "$snapshot_dir/"
cp -R "$datadir/rocksdb/backup" "$snapshot_dir/rocksdb"
"$NANO_NODE_EXE" --network dev --data_path "$snapshot_dir" --debug_block_count | grep -q 'Block count: 1'

node --vacuum
test -f "$datadir/rocksdb/CURRENT"
test -f "$datadir/rocksdb/backup/CURRENT"
node --debug_block_count | grep -q 'Block count: 1'
cmp "$datadir/reference.ldb" "$datadir/data.ldb"

# Hold the primary lock with a running dev node; both maintenance commands must preserve the completed backup.
reference_dir=$(new_datadir)
cp -R "$datadir/rocksdb/backup" "$reference_dir/backup"
ready="$datadir/maintenance-ready.json"
"$NANO_NODE_EXE" --daemon --network dev --data_path "$datadir" --runtime_info_file "$ready" \
    --config node.peering_port=0 > "$datadir/daemon.log" 2>&1 &
daemon_pid=$!
register_pid "$daemon_pid"
wait_for_file "$ready" "$daemon_pid"
for option in --snapshot --vacuum; do
    if node "$option"; then
        exit 1
    fi
    diff -r "$reference_dir/backup" "$datadir/rocksdb/backup"
done
interrupt_process "$daemon_pid"
wait "$daemon_pid"
node --debug_block_count | grep -q 'Block count: 1'

missing_dir=$(new_datadir)
cp "$datadir/config-node.toml" "$missing_dir/"
if "$NANO_NODE_EXE" --network dev --data_path "$missing_dir" --vacuum; then
    exit 1
fi
test ! -e "$missing_dir/rocksdb"
