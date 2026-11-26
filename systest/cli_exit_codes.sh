#!/bin/bash
set -eux

source "$(dirname "$0")/lib/common.sh"

DATADIR=$(new_datadir)

# commands that succeed exit with 0
$NANO_NODE_EXE --key_create --data_path $DATADIR > /dev/null
$NANO_NODE_EXE --generate_config node --data_path $DATADIR > /dev/null

# The logging diagnostic uses the daemon's sinks and severity settings before a ledger exists.
(
    unset NANO_LOG NANO_LOG_LEVELS
    logdir=$(new_datadir)
    printf '[log]\ndefault_level = "warn"\n[log.console]\nenable = false\n' > "$logdir/config-log.toml"
    "$NANO_NODE_EXE" --debug_sys_logging --network dev --data_path "$logdir" --config 'log.default_level="info"' > "$logdir/console" 2>&1
    if grep -Fq 'Testing system logger' "$logdir/console"; then exit 1; fi
    for severity in CRITICAL ERROR WARN INFO; do
        grep -Fq "Testing system logger ($severity)" "$logdir"/log/*.log
    done
    if grep -Fq 'Testing system logger (DEBUG)' "$logdir"/log/*.log; then exit 1; fi
)

# commands that print an error exit with 1, any other status such as a crash fails the test
expect_failure() {
    local status=0
    $NANO_NODE_EXE "$@" --data_path $DATADIR || status=$?
    if [ "$status" -ne 1 ]; then
        echo "FAIL: nano_node $* exited with $status, expected 1" >&2
        exit 1
    fi
}
expect_failure --key_expand
expect_failure --key_expand --key invalid
expect_failure --generate_config invalid

# A full output volume must prevent a caller from installing an incomplete configuration.
if [ -c /dev/full ]; then
    expect_failure --generate_config node > /dev/full
    expect_failure --update_config > /dev/full
fi

account=nano_1111111111111111111111111111111111111111111111111113b8661hfk
key=$(printf '%064x' 1)
[ "$("$NANO_NODE_EXE" --account_key --account "$account" --data_path "$DATADIR")" = "Hex: $key" ]
expect_failure --account_key --account "${account%?}1" > "$DATADIR/account_key.out"
[ ! -s "$DATADIR/account_key.out" ]

# An enabled monitor needs a positive interval to calculate rates.
printf '[node.monitor]\ninterval = 0\n' > "$DATADIR/config-node.toml"
expect_failure --update_config > "$DATADIR/monitor.out" 2> "$DATADIR/monitor.err"
grep -q 'monitor.interval' "$DATADIR/monitor.err"
printf 'enable = false\n' >> "$DATADIR/config-node.toml"
"$NANO_NODE_EXE" --update_config --data_path "$DATADIR" > /dev/null

# Manual backlog scans also need a nonempty batch when ongoing scanning is disabled.
printf '[node.backlog_scan]\nenable = false\nbatch_size = 0\n' > "$DATADIR/config-node.toml"
expect_failure --update_config > /dev/null 2> "$DATADIR/scan-error"
grep -Fq 'backlog_scan.batch_size' "$DATADIR/scan-error"
printf '[node.backlog_scan]\nbatch_size = 1\n' > "$DATADIR/config-node.toml"
"$NANO_NODE_EXE" --update_config --data_path "$DATADIR" > /dev/null

# a node config that cannot be parsed fails the commands that read it
echo "[node" > $DATADIR/config-node.toml
expect_failure --database_info

# if it got this far then it is a pass
exit 0
