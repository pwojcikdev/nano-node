#!/bin/bash
set -eu

network='live'

print_usage() {
	echo 'build.sh [-h] [-n {live|beta|dev}]'
}

while getopts 'hn:' OPT; do
	case "${OPT}" in
	h)
		print_usage
		exit 0
		;;
	n)
		network="${OPTARG}"
		;;
	*)
		print_usage >&2
		exit 1
		;;
	esac
done

case "${network}" in
live)
	network_tag=''
	;;
dev | beta)
	network_tag="-${network}"
	;;
*)
	echo "Invalid network: ${network}" >&2
	exit 1
	;;
esac

REPO_ROOT=$(git rev-parse --show-toplevel)
docker build --build-arg NANO_NETWORK="${network}" -f "$REPO_ROOT/docker/node/Dockerfile" -t "nano-node${network_tag}:latest" "$REPO_ROOT"
