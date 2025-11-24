#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/common.hpp>
#include <nano/store/fwd.hpp>
#include <nano/store/write_queue.hpp>

#include <filesystem>
#include <memory>

namespace nano::store
{
class ledger_store
{
public:
	explicit ledger_store (std::unique_ptr<nano::store::backend>, nano::store::open_mode mode, nano::stats & stats, nano::logger & logger);

	nano::store::write_transaction tx_begin_write ();
	nano::store::read_transaction tx_begin_read () const;

	bool empty (nano::store::transaction const &) const;
	void initialize (nano::store::write_transaction const &, nano::ledger_constants const & constants);
	void perform_upgrades ();

	std::string vendor_get () const;
	std::filesystem::path get_database_path () const;
	nano::store::open_mode get_mode () const;

private:
	std::unique_ptr<nano::store::backend> backend_impl;
	nano::store::backend & backend;

public: // TODO: Shouldn't be public
	nano::store::write_queue write_queue;

public:
	nano::stats & stats;
	nano::logger & logger;

private:
	std::unique_ptr<nano::store::ledger::block> block_impl;
	std::unique_ptr<nano::store::ledger::account> account_impl;
	std::unique_ptr<nano::store::ledger::pending> pending_impl;
	std::unique_ptr<nano::store::ledger::rep_weight> rep_weight_impl;
	std::unique_ptr<nano::store::ledger::online_weight> online_weight_impl;
	std::unique_ptr<nano::store::ledger::pruned> pruned_impl;
	std::unique_ptr<nano::store::ledger::peer> peer_impl;
	std::unique_ptr<nano::store::ledger::confirmation_height> confirmation_height_impl;
	std::unique_ptr<nano::store::ledger::final_vote> final_vote_impl;
	std::unique_ptr<nano::store::ledger::version> version_impl;

public:
	nano::store::ledger::block & block;
	nano::store::ledger::account & account;
	nano::store::ledger::pending & pending;
	nano::store::ledger::rep_weight & rep_weight;
	nano::store::ledger::online_weight & online_weight;
	nano::store::ledger::pruned & pruned;
	nano::store::ledger::peer & peer;
	nano::store::ledger::confirmation_height & confirmation_height;
	nano::store::ledger::final_vote & final_vote;
	nano::store::ledger::version & version;

public:
	static uint64_t constexpr version_minimum{ 21 };
	static uint64_t constexpr version_current{ 24 };

private:
	static nano::store::column_schema const schema_current;
};
};