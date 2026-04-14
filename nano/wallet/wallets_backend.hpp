#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/store/transaction.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/wallet/wallet_value.hpp>

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace nano::wallet
{
class wallets_backend
{
public:
	struct wallet_handle
	{
		nano::wallet_id id;
	};

	using iterator = nano::store::typed_iterator<nano::account, nano::wallet_value>;

public:
	virtual ~wallets_backend () = default;

	virtual nano::store::read_transaction tx_begin_read () const = 0;
	virtual nano::store::write_transaction tx_begin_write () = 0;

	virtual wallet_handle open_wallet (nano::store::write_transaction const &, nano::wallet_id const &) = 0;
	virtual void destroy_wallet (nano::store::write_transaction const &, wallet_handle const &) = 0;
	virtual std::vector<nano::wallet_id> wallet_ids (nano::store::transaction const &) const = 0;

	virtual nano::wallet_value get (nano::store::transaction const &, wallet_handle const &, nano::account const &) const = 0;
	virtual void put (nano::store::write_transaction const &, wallet_handle const &, nano::account const &, nano::wallet_value const &) = 0;
	virtual void del (nano::store::write_transaction const &, wallet_handle const &, nano::account const &) = 0;
	virtual iterator begin (nano::store::transaction const &, wallet_handle const &, nano::account const &) const = 0;
	virtual iterator end (nano::store::transaction const &, wallet_handle const &) const = 0;

	virtual std::optional<nano::block_hash> send_action_id_get (nano::store::transaction const &, std::string_view id) const = 0;
	virtual bool send_action_id_put (nano::store::write_transaction const &, std::string_view id, nano::block_hash const &) = 0;
	virtual void clear_send_action_ids (nano::store::write_transaction const &) = 0;

	virtual std::filesystem::path path () const = 0;
	virtual void backup (nano::logger &) = 0;
};
}
