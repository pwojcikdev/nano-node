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
# A maintenance write during a migration must force a retry. SIGSTOP lets us control the order.
if ! is_windows; then
    (
        for attempt in 1 2 3; do
            concurrent=$(new_datadir)
            cp "$datadir/data.ldb" "$datadir/config-node.toml" "$concurrent/"
            "$NANO_NODE_EXE" --data_path "$concurrent" --network dev --migrate_database_lmdb_to_rocksdb > "$concurrent/migration.log" 2>&1 &
            pid=$!
            trap 'kill -CONT "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true' EXIT
            staged="$concurrent/.lmdb-to-rocksdb/ledger/CURRENT"
            for i in {1..3000}; do
                if [ -f "$staged" ] || ! kill -0 "$pid" 2>/dev/null; then break; fi
                sleep 0.01
            done
            if [ -f "$staged" ] && kill -STOP "$pid" 2>/dev/null && [ -f "$staged" ]; then
                "$NANO_NODE_EXE" --data_path "$concurrent" --network dev --confirmation_height_clear --account all
                kill -CONT "$pid"
                if wait "$pid"; then exit 1; fi
                trap - EXIT
                grep -Fq 'LMDB ledger changed during migration' "$concurrent/migration.log"
                test ! -e "$concurrent/rocksdb"
                "$NANO_NODE_EXE" --data_path "$concurrent" --network dev --migrate_database_lmdb_to_rocksdb
                "$NANO_NODE_EXE" --data_path "$concurrent" --network dev --config node.database_backend=rocksdb --debug_block_count | grep -q 'Block count: 1'
                exit 0
            fi
            # Try a fresh copy if this small migration completed before we could pause it.
            kill -CONT "$pid" 2>/dev/null || true
            wait "$pid"
            trap - EXIT
        done
        echo 'Could not pause the migration to test a concurrent LMDB write' >&2
        exit 1
    )
fi
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

# A live LMDB daemon can still serve snapshots, but its ledger cannot be replaced.
ready="$datadir/lmdb-runtime.json"
"$NANO_NODE_EXE" --data_path "$datadir" --network dev --daemon --runtime_info_file "$ready" --config node.peering_port=0 > "$datadir/lmdb-daemon.log" 2>&1 &
lmdb_pid=$!
register_pid "$lmdb_pid"
wait_for_file "$ready" "$lmdb_pid"
rm -f "$datadir/snapshot.ldb"
node --snapshot
if node --vacuum --peer_clear > "$datadir/vacuum-error" 2>&1; then
    exit 1
fi
grep -q 'Stop the node before vacuuming the LMDB ledger' "$datadir/vacuum-error"
interrupt_process "$lmdb_pid"
wait "$lmdb_pid"

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
# A vacuum must keep the live RocksDB lock file in place throughout the operation.
lock_copy="$datadir/rocksdb-lock-reference"
ln "$datadir/rocksdb/LOCK" "$lock_copy" 2>/dev/null || true
node --vacuum
if [ -e "$lock_copy" ]; then
    test "$lock_copy" -ef "$datadir/rocksdb/LOCK"
fi
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

# Cleanup options need write access, but backup commands still require an existing ledger.
for backend in lmdb rocksdb; do
    missing_dir=$(new_datadir)
    printf '[node]\ndatabase_backend = "%s"\n' "$backend" > "$missing_dir/config-node.toml"
    for operation in --snapshot --vacuum; do
        for cleanup in '' '--peer_clear'; do
            if "$NANO_NODE_EXE" --network dev --data_path "$missing_dir" "$operation" ${cleanup:+"$cleanup"}; then
                exit 1
            fi
            test ! -e "$missing_dir/data.ldb"
            test ! -e "$missing_dir/snapshot.ldb"
            test ! -e "$missing_dir/rocksdb"
        done
    done
done
