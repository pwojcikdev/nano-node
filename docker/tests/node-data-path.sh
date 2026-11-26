#!/usr/bin/env bash
set -eu

# Run inside the production image with its installed entrypoint and config files.
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
export NANO_APP_PATH="$work/network data"
selected="$work/bind mount"

mkdir -p "$NANO_APP_PATH/RaiBlocksDev"
touch "$NANO_APP_PATH/RaiBlocksDev/legacy-marker"
entry.sh --initialize --network=DeV --data_path "$selected"
test -f "$NANO_APP_PATH/RaiBlocksDev/legacy-marker"
entry.sh --initialize --network dev --data_path="$NANO_APP_PATH/NanoDev/"
test -f "$NANO_APP_PATH/NanoDev/legacy-marker"
entry.sh --initialize --network dev
for dir in "$selected" "$NANO_APP_PATH/NanoDev"; do
	test -f "$dir/data.ldb"
	cmp /usr/share/nano/config/config-node.toml "$dir/config-node.toml"
	cmp /usr/share/nano/config/config-rpc.toml "$dir/config-rpc.toml"
done
test ! -e "$NANO_APP_PATH/Nano"

# A sparse file crosses the startup threshold without using a GiB of disk.
truncate -s 1073741825 "$selected/data.ldb"
entry.sh --daemon --network dev --data_path="$selected" --vacuum-over 1 \
	--config node.peering_port=0 node.websocket.port=0 --rpcconfig port=0 \
	--runtime_info_file "$work/ready.json" &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" || true; rm -rf -- "$work"' EXIT
for ((i=0; i<300; i++)); do
	[[ ! -s "$work/ready.json" ]] || break
	kill -0 "$pid"
	sleep 0.1
done
grep -q '"rpc_port"' "$work/ready.json"
test -f "$selected/backup.vacuum.ldb"
test "$(stat -c %s "$selected/data.ldb")" -lt 1073741824
kill "$pid"
wait "$pid"
trap 'rm -rf -- "$work"' EXIT
entry.sh --debug_block_count --network dev --data_path "$selected" | grep -q 'Block count: 1'
