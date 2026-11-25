#!/bin/bash
set -eux

source "$(dirname "$0")/lib/common.sh"

DATADIR=$(new_datadir)

SEED=CEEDCEEDCEEDCEEDCEEDCEEDCEEDCEEDCEEDCEEDCEEDCEEDCEEDCEEDCEEDCEED

# initialise data directory
$NANO_NODE_EXE --initialize --data_path $DATADIR

# create a wallet and store the wallet ID
wallet_id=`$NANO_NODE_EXE --wallet_create --data_path $DATADIR --seed $SEED`

# decrypt the wallet and check the seed
$NANO_NODE_EXE --wallet_decrypt_unsafe --wallet $wallet_id --data_path $DATADIR | grep -q "Seed: $SEED"

# list the wallet and check the wallet ID
$NANO_NODE_EXE --wallet_list --data_path $DATADIR | grep -q "Wallet ID: $wallet_id"

# Verify the password written by offline RPC with a separate wallet command.
"$NANO_NODE_EXE" --debug_rpc --data_path "$DATADIR" <<JSON | jq -e '.changed == "1"' > /dev/null
{
    "action": "password_change",
    "wallet": "$wallet_id",
    "password": "two words"
}
JSON
"$NANO_NODE_EXE" --account_create --data_path "$DATADIR" --wallet "$wallet_id" --password "two words" > /dev/null

# if it got this far then it is a pass
exit 0
