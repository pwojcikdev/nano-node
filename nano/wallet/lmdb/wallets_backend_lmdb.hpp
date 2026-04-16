#pragma once

#include <nano/lib/lmdbconfig.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/store/lmdb/lmdb_env.hpp>
#include <nano/wallet/wallets_backend.hpp>

#include <unordered_map>

namespace nano::wallet
{
class wallets_backend_lmdb final : public wallets_backend
{
public:
	wallets_backend_lmdb (std::filesystem::path const &, nano::lmdb_config const & = nano::lmdb_config{});

	nano::store::read_transaction tx_begin_read () const override;
	nano::store::write_transaction tx_begin_write () override;

	wallet_handle open_wallet (nano::store::write_transaction const &, nano::wallet_id const &) override;
	void destroy_wallet (nano::store::write_transaction const &, wallet_handle const &) override;
	std::vector<nano::wallet_id> wallet_ids (nano::store::transaction const &) const override;

	wallet_value get (nano::store::transaction const &, wallet_handle const &, nano::account const &) const override;
	void put (nano::store::write_transaction const &, wallet_handle const &, nano::account const &, wallet_value const &) override;
	void del (nano::store::write_transaction const &, wallet_handle const &, nano::account const &) override;
	iterator begin (nano::store::transaction const &, wallet_handle const &, nano::account const &) const override;
	iterator end (nano::store::transaction const &, wallet_handle const &) const override;

	std::optional<nano::block_hash> send_action_id_get (nano::store::transaction const &, std::string_view id) const override;
	bool send_action_id_put (nano::store::write_transaction const &, std::string_view id, nano::block_hash const &) override;
	void clear_send_action_ids (nano::store::write_transaction const &) override;

	std::filesystem::path path () const override;
	void backup (nano::logger &) override;

private:
	auto table_handle (wallet_handle const &) const -> MDB_dbi;
	auto open_wallet_handle (nano::store::transaction const &, nano::wallet_id const &, bool create) -> MDB_dbi;

private:
	nano::store::lmdb::env environment;
	MDB_dbi main_handle{};
	MDB_dbi send_action_ids_handle{};
	mutable nano::mutex mutex;
	std::unordered_map<nano::wallet_id, MDB_dbi> wallet_handles;
};
}
