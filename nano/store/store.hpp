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
	explicit ledger_store (std::unique_ptr<store::backend>, nano::stats & stats, nano::logger & logger);

	store::write_transaction tx_begin_write ();
	store::read_transaction tx_begin_read () const;

	std::string vendor_get () const;
	std::filesystem::path get_database_path () const;
	nano::store::open_mode get_mode () const;

private:
	std::unique_ptr<store::backend> backend_impl;
	store::backend & backend;

public: // TODO: Shouldn't be public
	store::write_queue write_queue;

public:
	nano::stats & stats;
	nano::logger & logger;

private:
	std::unique_ptr<store::ledger::block> block_impl;
	std::unique_ptr<store::ledger::account> account_impl;
	std::unique_ptr<store::ledger::pending> pending_impl;
	std::unique_ptr<store::ledger::rep_weight> rep_weight_impl;
	std::unique_ptr<store::ledger::online_weight> online_weight_impl;
	std::unique_ptr<store::ledger::pruned> pruned_impl;
	std::unique_ptr<store::ledger::peer> peer_impl;
	std::unique_ptr<store::ledger::confirmation_height> confirmation_height_impl;
	std::unique_ptr<store::ledger::final_vote> final_vote_impl;
	std::unique_ptr<store::ledger::version> version_impl;

public:
	store::ledger::block & block;
	store::ledger::account & account;
	store::ledger::pending & pending;
	store::ledger::rep_weight & rep_weight;
	store::ledger::online_weight & online_weight;
	store::ledger::pruned & pruned;
	store::ledger::peer & peer;
	store::ledger::confirmation_height & confirmation_height;
	store::ledger::final_vote & final_vote;
	store::ledger::version & version;

public:
	static uint64_t constexpr version_minimum{ 21 };
	static uint64_t constexpr version_current{ 24 };
};
};