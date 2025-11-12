#pragma once

#include <nano/lib/errors.hpp>
#include <nano/lib/numbers.hpp>

#include <chrono>

namespace nano
{
class tomlconfig;

class vote_storage_config final
{
public:
	nano::error deserialize (nano::tomlconfig &);
	nano::error serialize (nano::tomlconfig &) const;

public:
	/** Enable vote storage and rebroadcasting */
	bool enable{ true };

	/** Minimum weight of votes to store and broadcast (default: ~ quorum) */
	nano::amount vote_weight_threshold{ 60000 * nano::Knano_ratio };

	/** Minimum weight for final votes to store and broadcast */
	nano::amount vote_final_weight_threshold{ 60000 * nano::Knano_ratio };

	/** Minimum representative weight to trigger broadcasts */
	nano::amount rep_weight_threshold{ 100 * nano::nano_ratio };

	/** Only trigger broadcasts to principal representatives */
	bool trigger_pr_only{ true };

	/** Only store final votes (timestamp == max) */
	bool store_final_only{ true };

	/** Ignore large votes  */
	bool ignore_255_votes{ false };

	/** Maximum busy ratio before dropping vote storage requests (0.0 - 1.0) */
	float max_busy_ratio{ 0.5f };

	/** Maximum age of trigger requests to process (older requests are dropped) */
	std::chrono::seconds request_age_cutoff{ 30 };

	/** Time window for tracking recently broadcasted hashes to prevent duplicates */
	std::chrono::seconds recently_broadcasted_window{ 10 };

	/** Maximum number of store queue items */
	size_t max_store_queue{ 1024 * 16 };

	/** Maximum number of reply queue items */
	size_t max_reply_queue{ 1024 * 8 };

	/** Maximum number of broadcast aggregation items */
	size_t max_broadcast_queue{ 1024 * 4 };

	/** Enable vote broadcast for most requested hashes */
	bool enable_broadcast{ false };

	/** Enable vote replies to requesting peers */
	bool enable_replies{ true };

	/** Enable broadcasting to principal representatives */
	bool enable_pr_broadcast{ true };

	/** Enable random broadcast (experimental) */
	bool enable_random_broadcast{ false };

	/** Enable frontier query optimization (experimental) */
	bool enable_query_frontier{ false };
};
}
