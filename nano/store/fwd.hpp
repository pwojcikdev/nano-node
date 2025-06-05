#pragma once

namespace nano
{
enum class tables;
}
namespace nano::store
{
class backend;
class ledger_store;
class account;
class block;
class component;
class confirmation_height;
class final_vote;
class online_weight;
class peer;
class pending;
class pruned;
class read_transaction;
class rep_weight;
class transaction;
class version;
class write_transaction;
}
namespace nano::store::ledger
{
class account_view;
class block_view;
class confirmation_height_view;
class final_vote_view;
class online_weight_view;
class peer_view;
class pending_view;
class pruned_view;
class rep_weight_view;
class version_view;
}