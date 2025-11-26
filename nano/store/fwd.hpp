#pragma once

namespace nano
{
enum class tables;
}

namespace nano::store
{
class backend;
class ledger_store;
class read_transaction;
class transaction;
class write_transaction;
}

namespace nano::store::ledger
{
class account;
class block;
class confirmation_height;
class final_vote;
class online_weight;
class peer;
class pending;
class pruned;
class rep_weight;
class version;
}