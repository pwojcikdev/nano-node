#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/errors.hpp>
#include <nano/lib/json_error_response.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/rpc_handler_interface.hpp>
#include <nano/lib/rpcconfig.hpp>
#include <nano/rpc/rpc_dispatcher.hpp>

#include <boost/property_tree/json_parser.hpp>

#include <unordered_set>

namespace
{
std::unordered_set<std::string> create_rpc_control_impls ();
std::unordered_set<std::string> rpc_control_impl_set = create_rpc_control_impls ();
std::string filter_request (boost::property_tree::ptree tree_a);
}

nano::rpc_dispatcher::rpc_dispatcher (nano::rpc_config const & rpc_config, std::string const & body_a, std::string const & request_id_a, std::function<void (std::string const &)> const & response_a, nano::rpc_handler_interface & rpc_handler_interface_a, nano::logger & logger) :
	body (body_a),
	request_id (request_id_a),
	response (response_a),
	rpc_config (rpc_config),
	rpc_handler_interface (rpc_handler_interface_a),
	logger (logger)
{
}

void nano::rpc_dispatcher::process_request (nano::rpc_handler_request_params const & request_params)
{
	try
	{
		if (exceeds_max_depth ())
		{
			json_error_response (response, "Max JSON depth exceeded");
		}
		else if (request_params.rpc_version == 1)
		{
			process_v1 ();
		}
		else if (request_params.rpc_version == 2)
		{
			process_v2 (request_params);
		}
		else
		{
			debug_assert (false);
			json_error_response (response, "Invalid RPC version");
		}
	}
	catch (std::exception const & ex)
	{
		// A request that cannot be parsed is answered where it is parsed, so what arrives here is a failure of the server, the backend included
		logger.error (nano::log::type::rpc_request, "Request {} failed: {}", request_id, ex.what ());
		json_error_response (response, "Internal server error in RPC");
	}
	catch (...)
	{
		logger.error (nano::log::type::rpc_request, "Request {} failed: unknown error", request_id);
		json_error_response (response, "Internal server error in RPC");
	}
}

void nano::rpc_dispatcher::process_v1 ()
{
	boost::property_tree::ptree request;
	try
	{
		std::stringstream stream{ body };
		boost::property_tree::read_json (stream, request);
	}
	catch (boost::property_tree::json_parser_error const &)
	{
		json_error_response (response, "Unable to parse JSON");
		return;
	}

	auto const action = request.get_optional<std::string> ("action");
	if (!action)
	{
		json_error_response (response, "Unable to parse JSON"); // What the node itself answers to a request without an action
		return;
	}

	// Bump logging level if RPC request logging is enabled
	logger.log (rpc_config.rpc_logging.log_rpc ? nano::log::level::info : nano::log::level::debug,
	nano::log::type::rpc_request, "Request {} : {}", request_id, filter_request (request));

	if (!rpc_config.enable_control && requires_control (*action, request))
	{
		std::error_code const rpc_control_disabled_ec = nano::error_rpc::rpc_control_disabled;
		json_error_response (response, rpc_control_disabled_ec.message ());
		return;
	}

	// Add random id to RPC send via IPC if not included
	if (*action == "send" && request.find ("id") == request.not_found ())
	{
		nano::uint128_union random_id;
		nano::random_pool::generate_block (random_id.bytes.data (), random_id.bytes.size ());
		request.put ("id", random_id.to_string ());
		std::stringstream ostream;
		boost::property_tree::write_json (ostream, request);
		body = ostream.str ();
	}

	rpc_handler_interface.process_request (*action, body, response);
}

void nano::rpc_dispatcher::process_v2 (nano::rpc_handler_request_params const & request_params)
{
	rpc_handler_interface.process_request_v2 (request_params, body, [response = response] (std::shared_ptr<std::string> const & body) {
		response (*body);
	});
}

bool nano::rpc_dispatcher::exceeds_max_depth () const
{
	auto depth (0u);
	for (auto ch : body)
	{
		if (ch == '[' || ch == '{')
		{
			if (depth >= rpc_config.max_json_depth)
			{
				return true;
			}
			++depth;
		}
	}
	return false;
}

bool nano::rpc_dispatcher::requires_control (std::string const & action, boost::property_tree::ptree const & request) const
{
	if (rpc_control_impl_set.contains (action))
	{
		return true;
	}
	// Two actions are gated on a parameter; one that is missing or malformed is left to the backend to complain about
	if (action == "stats")
	{
		return request.get<std::string> ("type", "") == "objects";
	}
	if (action == "process")
	{
		return request.get_optional<bool> ("force").value_or (false);
	}
	return false;
}

namespace
{
std::unordered_set<std::string> create_rpc_control_impls ()
{
	std::unordered_set<std::string> set;
	set.emplace ("account_create");
	set.emplace ("account_move");
	set.emplace ("account_remove");
	set.emplace ("account_representative_set");
	set.emplace ("accounts_create");
	set.emplace ("block_create");
	set.emplace ("bootstrap_lazy");
	set.emplace ("bootstrap_reset");
	set.emplace ("bootstrap_priorities");
	set.emplace ("database_txn_tracker");
	set.emplace ("epoch_upgrade");
	set.emplace ("keepalive");
	set.emplace ("ledger");
	set.emplace ("node_id");
	set.emplace ("password_change");
	set.emplace ("password_enter");
	set.emplace ("populate_backlog");
	set.emplace ("receive");
	set.emplace ("receive_minimum");
	set.emplace ("receive_minimum_set");
	set.emplace ("search_pending");
	set.emplace ("search_receivable");
	set.emplace ("search_pending_all");
	set.emplace ("search_receivable_all");
	set.emplace ("send");
	set.emplace ("stats_clear");
	set.emplace ("stop");
	set.emplace ("unchecked_clear");
	set.emplace ("unopened");
	set.emplace ("wallet_add");
	set.emplace ("wallet_add_watch");
	set.emplace ("wallet_change_seed");
	set.emplace ("wallet_create");
	set.emplace ("wallet_destroy");
	set.emplace ("wallet_export");
	set.emplace ("wallet_lock");
	set.emplace ("wallet_representative_set");
	set.emplace ("wallet_republish");
	set.emplace ("wallet_unlock");
	set.emplace ("wallet_work_get");
	set.emplace ("work_generate");
	set.emplace ("work_cancel");
	set.emplace ("work_get");
	set.emplace ("work_set");
	set.emplace ("work_peer_add");
	set.emplace ("work_peers");
	set.emplace ("work_peers_clear");
	set.emplace ("wallet_seed");
	return set;
}

std::string filter_request (boost::property_tree::ptree tree_a)
{
	// Replace password
	auto password_text (tree_a.get_optional<std::string> ("password"));
	if (password_text.has_value ())
	{
		tree_a.put ("password", "password");
	}
	// Save first 2 symbols of wallet, key, seed
	auto wallet_text (tree_a.get_optional<std::string> ("wallet"));
	if (wallet_text.has_value () && wallet_text.value ().length () > 2)
	{
		tree_a.put ("wallet", wallet_text.value ().replace (wallet_text.value ().begin () + 2, wallet_text.value ().end (), wallet_text.value ().length () - 2, 'X'));
	}
	auto key_text (tree_a.get_optional<std::string> ("key"));
	if (key_text.has_value () && key_text.value ().length () > 2)
	{
		tree_a.put ("key", key_text.value ().replace (key_text.value ().begin () + 2, key_text.value ().end (), key_text.value ().length () - 2, 'X'));
	}
	auto seed_text (tree_a.get_optional<std::string> ("seed"));
	if (seed_text.has_value () && seed_text.value ().length () > 2)
	{
		tree_a.put ("seed", seed_text.value ().replace (seed_text.value ().begin () + 2, seed_text.value ().end (), seed_text.value ().length () - 2, 'X'));
	}
	std::string result;
	std::stringstream stream;
	boost::property_tree::write_json (stream, tree_a, false);
	result = stream.str ();
	// removing std::endl
	if (result.length () > 1)
	{
		result.pop_back ();
	}
	return result;
}
}
