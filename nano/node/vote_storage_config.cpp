#include <nano/lib/tomlconfig.hpp>
#include <nano/node/vote_storage_config.hpp>

nano::error nano::vote_storage_config::serialize (nano::tomlconfig & toml) const
{
	toml.put ("enable", enable, "Enable vote storage and rebroadcasting.\ntype:bool");
	toml.put ("vote_weight_threshold", vote_weight_threshold.to_string_dec (), "Minimum weight of votes to store and broadcast (in raw).\ntype:string,amount,raw");
	toml.put ("vote_final_weight_threshold", vote_final_weight_threshold.to_string_dec (), "Minimum weight for final votes to store and broadcast (in raw).\ntype:string,amount,raw");
	toml.put ("rep_weight_threshold", rep_weight_threshold.to_string_dec (), "Minimum representative weight to trigger broadcasts (in raw).\ntype:string,amount,raw");
	toml.put ("trigger_pr_only", trigger_pr_only, "Only trigger broadcasts to principal representatives.\ntype:bool");
	toml.put ("store_final_only", store_final_only, "Only store final votes (timestamp == max).\ntype:bool");
	toml.put ("ignore_255_votes", ignore_255_votes, "Ignore votes with timestamp 255 (hinted votes).\ntype:bool");
	toml.put ("max_busy_ratio", max_busy_ratio, "Maximum busy ratio before dropping vote storage requests (0.0 - 1.0).\ntype:float");
	toml.put ("request_age_cutoff", request_age_cutoff.count (), "Maximum age of trigger requests to process in seconds.\ntype:uint64");
	toml.put ("recently_broadcasted_window", recently_broadcasted_window.count (), "Time window for tracking recently broadcasted hashes to prevent duplicates (seconds).\ntype:uint64");
	toml.put ("max_store_queue", max_store_queue, "Maximum number of store queue items.\ntype:uint64");
	toml.put ("max_reply_queue", max_reply_queue, "Maximum number of reply queue items.\ntype:uint64");
	toml.put ("max_broadcast_queue", max_broadcast_queue, "Maximum number of broadcast aggregation items.\ntype:uint64");
	toml.put ("enable_broadcast", enable_broadcast, "Enable vote broadcast to principal representatives.\ntype:bool");
	toml.put ("enable_replies", enable_replies, "Enable vote replies to requesting peers.\ntype:bool");
	toml.put ("enable_pr_broadcast", enable_pr_broadcast, "Enable broadcasting to principal representatives.\ntype:bool");
	toml.put ("enable_random_broadcast", enable_random_broadcast, "Enable random broadcast (experimental).\ntype:bool");
	toml.put ("enable_query_frontier", enable_query_frontier, "Enable frontier query optimization (experimental).\ntype:bool");

	return toml.get_error ();
}

nano::error nano::vote_storage_config::deserialize (nano::tomlconfig & toml)
{
	toml.get ("enable", enable);

	auto vote_weight_threshold_l = vote_weight_threshold.to_string_dec ();
	toml.get ("vote_weight_threshold", vote_weight_threshold_l);
	if (vote_weight_threshold.decode_dec (vote_weight_threshold_l))
	{
		toml.get_error ().set ("vote_weight_threshold contains an invalid decimal amount");
	}

	auto vote_final_weight_threshold_l = vote_final_weight_threshold.to_string_dec ();
	toml.get ("vote_final_weight_threshold", vote_final_weight_threshold_l);
	if (vote_final_weight_threshold.decode_dec (vote_final_weight_threshold_l))
	{
		toml.get_error ().set ("vote_final_weight_threshold contains an invalid decimal amount");
	}

	auto rep_weight_threshold_l = rep_weight_threshold.to_string_dec ();
	toml.get ("rep_weight_threshold", rep_weight_threshold_l);
	if (rep_weight_threshold.decode_dec (rep_weight_threshold_l))
	{
		toml.get_error ().set ("rep_weight_threshold contains an invalid decimal amount");
	}

	toml.get ("trigger_pr_only", trigger_pr_only);
	toml.get ("store_final_only", store_final_only);
	toml.get ("ignore_255_votes", ignore_255_votes);
	toml.get ("max_busy_ratio", max_busy_ratio);

	auto request_age_cutoff_l = request_age_cutoff.count ();
	toml.get ("request_age_cutoff", request_age_cutoff_l);
	request_age_cutoff = std::chrono::seconds (request_age_cutoff_l);

	auto recently_broadcasted_window_l = recently_broadcasted_window.count ();
	toml.get ("recently_broadcasted_window", recently_broadcasted_window_l);
	recently_broadcasted_window = std::chrono::seconds (recently_broadcasted_window_l);

	toml.get ("max_store_queue", max_store_queue);
	toml.get ("max_reply_queue", max_reply_queue);
	toml.get ("max_broadcast_queue", max_broadcast_queue);
	toml.get ("enable_broadcast", enable_broadcast);
	toml.get ("enable_replies", enable_replies);
	toml.get ("enable_pr_broadcast", enable_pr_broadcast);
	toml.get ("enable_random_broadcast", enable_random_broadcast);
	toml.get ("enable_query_frontier", enable_query_frontier);

	return toml.get_error ();
}
