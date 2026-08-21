#include <nano/boost/asio/bind_executor.hpp>
#include <nano/boost/asio/ip/tcp.hpp>
#include <nano/boost/asio/strand.hpp>
#include <nano/boost/beast/http.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/network_formatting.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/thread_pool.hpp>
#include <nano/lib/work.hpp>
#include <nano/node/endpoint.hpp>
#include <nano/node/node_observers.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/work_generator.hpp>
#include <nano/secure/network_params.hpp>

#include <boost/algorithm/string/erase.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <sstream>

#include <fmt/format.h>

/*
 * generation
 */

// A single in-flight generation racing the local work pool against the requested work peers, the first valid result wins
class nano::work_generator::generation final : public std::enable_shared_from_this<nano::work_generator::generation>
{
public:
	enum class outcome
	{
		success,
		cancelled,
		local_failure,
		peers_failure,
	};

	generation (nano::work_generator & generator_a, nano::work_request request_a, bool use_local_a) :
		request{ std::move (request_a) },
		generator{ generator_a },
		use_local{ use_local_a },
		pending{ request.peers.size () },
		strand{ generator_a.io_ctx.get_executor () },
		resolver{ generator_a.io_ctx }
	{
	}

	void start ()
	{
		if (use_local)
		{
			start_local ();
		}
		for (auto const & peer : request.peers)
		{
			boost::system::error_code ec;
			auto address = boost::asio::ip::make_address_v6 (peer.address, ec);
			if (!ec)
			{
				request_peer (nano::tcp_endpoint{ address, peer.port });
			}
			else
			{
				resolve_peer (peer);
			}
		}
	}

	void cancel ()
	{
		if (complete (outcome::cancelled))
		{
			abort_requests (true);
		}
	}

public:
	nano::work_request const request;

private:
	// A single HTTP conversation with a work peer
	struct peer_connection final
	{
		peer_connection (boost::asio::io_context & io_ctx, nano::tcp_endpoint endpoint_a) :
			endpoint{ endpoint_a },
			socket{ io_ctx }
		{
		}

		nano::tcp_endpoint const endpoint;
		boost::asio::ip::tcp::socket socket;
		boost::beast::flat_buffer buffer;
		boost::beast::http::response<boost::beast::http::string_body> response;
	};

	void start_local ()
	{
		local_started = true;
		generator.work_pool.generate (request.version, request.root, request.difficulty, [this_l = shared_from_this ()] (std::optional<uint64_t> const & work) {
			if (work.has_value ())
			{
				this_l->complete (outcome::success, work.value (), "local");
			}
			else
			{
				this_l->complete (outcome::local_failure);
			}
			this_l->abort_requests (false);
		});
	}

	void resolve_peer (nano::work_peer const & peer)
	{
		resolver.async_resolve (peer.address, std::to_string (peer.port), [this_l = shared_from_this (), peer] (boost::system::error_code const & ec, boost::asio::ip::tcp::resolver::results_type results) {
			if (!ec)
			{
				// A single hostname can resolve to multiple endpoints, all of them are queried
				bool first = true;
				for (auto const & result : results)
				{
					if (!first)
					{
						++this_l->pending;
					}
					first = false;
					this_l->request_peer (nano::tcp_endpoint{ result.endpoint ().address (), result.endpoint ().port () });
				}
			}
			else
			{
				this_l->generator.stats.inc (nano::stat::type::work_generator, nano::stat::detail::resolve_error);
				this_l->generator.logger.error (nano::log::type::work_generator, "Error resolving work peer: {}:{} ({})", peer.address, peer.port, ec.message ());

				this_l->peer_failure ();
			}
		});
	}

	void request_peer (nano::tcp_endpoint const & endpoint)
	{
		auto connection = std::make_shared<peer_connection> (generator.io_ctx, endpoint);
		{
			nano::lock_guard<nano::mutex> guard{ mutex };
			connections.push_back (connection);
		}
		connection->socket.async_connect (connection->endpoint,
		boost::asio::bind_executor (strand, [this_l = shared_from_this (), connection] (boost::system::error_code const & ec) {
			if (!ec && !this_l->aborted)
			{
				this_l->send_request (connection);
			}
			else if (ec && ec != boost::system::errc::operation_canceled)
			{
				this_l->generator.stats.inc (nano::stat::type::work_generator, nano::stat::detail::peer_error);
				this_l->generator.logger.error (nano::log::type::work_generator, "Unable to connect to work peer {} ({})", connection->endpoint, ec.message ());

				this_l->record_bad_peer (connection->endpoint);
				this_l->peer_failure ();
			}
		}));
	}

	void send_request (std::shared_ptr<peer_connection> const & connection)
	{
		auto http_request = prepare_http_request (connection->endpoint, body_generate ());
		boost::beast::http::async_write (connection->socket, *http_request,
		boost::asio::bind_executor (strand, [this_l = shared_from_this (), connection, http_request] (boost::system::error_code const & ec, std::size_t) {
			if (!ec && !this_l->aborted)
			{
				this_l->read_response (connection);
			}
			else if (ec && ec != boost::system::errc::operation_canceled)
			{
				this_l->generator.stats.inc (nano::stat::type::work_generator, nano::stat::detail::peer_error);
				this_l->generator.logger.error (nano::log::type::work_generator, "Unable to write to work peer {} ({})", connection->endpoint, ec.message ());

				this_l->record_bad_peer (connection->endpoint);
				this_l->peer_failure ();
			}
		}));
	}

	void read_response (std::shared_ptr<peer_connection> const & connection)
	{
		boost::beast::http::async_read (connection->socket, connection->buffer, connection->response,
		boost::asio::bind_executor (strand, [this_l = shared_from_this (), connection] (boost::system::error_code const & ec, std::size_t) {
			if (!ec && !this_l->aborted)
			{
				this_l->handle_response (connection);
			}
			else if (ec)
			{
				// The peer might still be generating, tell it to stop
				this_l->send_cancel (connection->endpoint);
				this_l->peer_failure ();
			}
		}));
	}

	void handle_response (std::shared_ptr<peer_connection> const & connection)
	{
		auto const & endpoint = connection->endpoint;
		if (connection->response.result () != boost::beast::http::status::ok)
		{
			generator.stats.inc (nano::stat::type::work_generator, nano::stat::detail::bad_response);
			generator.logger.error (nano::log::type::work_generator, "Work peer {} responded with status {}", endpoint, connection->response.result_int ());

			record_bad_peer (endpoint);
			peer_failure ();
			return;
		}
		auto work = parse_work (connection->response.body (), endpoint);
		if (!work.has_value ())
		{
			generator.stats.inc (nano::stat::type::work_generator, nano::stat::detail::bad_response);
			record_bad_peer (endpoint);
			peer_failure ();
			return;
		}
		if (generator.network_params.work.difficulty (request.version, request.root, work.value ()) < request.difficulty)
		{
			generator.stats.inc (nano::stat::type::work_generator, nano::stat::detail::invalid_work);
			generator.logger.error (nano::log::type::work_generator, "Incorrect work response from {} for root {} with difficulty {}: {}", endpoint, request.root, nano::to_string_hex (request.difficulty), nano::to_string_hex (work.value ()));

			record_bad_peer (endpoint);
			peer_failure ();
			return;
		}
		// First valid response wins
		generator.unresponsive_peers = false;
		if (complete (outcome::success, work.value (), fmt::format ("{}:{}", endpoint.address ().to_string (), endpoint.port ())))
		{
			abort_requests (true);
		}
	}

	std::optional<uint64_t> parse_work (std::string const & body, nano::tcp_endpoint const & endpoint) const
	{
		try
		{
			std::stringstream istream{ body };
			boost::property_tree::ptree ptree;
			boost::property_tree::read_json (istream, ptree);
			auto work_text = ptree.get<std::string> ("work");
			uint64_t work;
			if (nano::from_string_hex (work_text, work))
			{
				generator.logger.error (nano::log::type::work_generator, "Work response from {} wasn't a number: {}", endpoint, work_text);
				return std::nullopt;
			}
			return work;
		}
		catch (...)
		{
			generator.logger.error (nano::log::type::work_generator, "Work response from {} wasn't parsable: {}", endpoint, body);
			return std::nullopt;
		}
	}

	// Sends a work_cancel message over a new connection, fire and forget
	void send_cancel (nano::tcp_endpoint const & endpoint)
	{
		auto connection = std::make_shared<peer_connection> (generator.io_ctx, endpoint);
		connection->socket.async_connect (connection->endpoint,
		boost::asio::bind_executor (strand, [this_l = shared_from_this (), connection] (boost::system::error_code const & ec) {
			if (!ec)
			{
				auto http_request = prepare_http_request (connection->endpoint, this_l->body_cancel ());
				boost::beast::http::async_write (connection->socket, *http_request,
				boost::asio::bind_executor (this_l->strand, [this_l, connection, http_request] (boost::system::error_code const & ec, std::size_t) {
					if (ec && ec != boost::system::errc::operation_canceled)
					{
						this_l->generator.logger.error (nano::log::type::work_generator, "Unable to send work cancel to work peer {} ({})", connection->endpoint, ec.message ());
					}
				}));
			}
		}));
	}

	void record_bad_peer (nano::tcp_endpoint const & endpoint)
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		bad_peers.push_back (fmt::format ("{}:{}", endpoint.address ().to_string (), endpoint.port ()));
	}

	// Marks one peer endpoint as failed, completes the generation once all of them have failed
	void peer_failure ()
	{
		if (++failures == pending.load () && !finished)
		{
			// Peers only rejoin the race for subsequent requests after a valid response
			generator.unresponsive_peers = true;
			if (!local_started)
			{
				complete (outcome::peers_failure);
			}
			// Otherwise the local work pool decides the outcome
		}
	}

	// Settles the generation exactly once, returns true for the deciding caller
	bool complete (outcome outcome_a, uint64_t work = 0, std::string winner = {})
	{
		if (finished.exchange (true))
		{
			return false;
		}
		nano::work_generation_result result;
		result.request = request;
		result.status = to_status (outcome_a);
		result.work = work;
		result.winner = std::move (winner);
		result.duration = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - start_time);
		{
			nano::lock_guard<nano::mutex> guard{ mutex };
			result.bad_peers = bad_peers;
		}
		generator.finished (*this, std::move (result), outcome_a == outcome::peers_failure);
		return true;
	}

	// Cancels local generation if requested and closes all open peer connections
	void abort_requests (bool cancel_local)
	{
		if (aborted.exchange (true))
		{
			return;
		}
		if (cancel_local && generator.local_enabled ())
		{
			generator.work_pool.cancel (request.root);
		}
		decltype (connections) to_close;
		{
			nano::lock_guard<nano::mutex> guard{ mutex };
			to_close.swap (connections);
		}
		for (auto const & connection : to_close)
		{
			boost::asio::post (strand, [this_l = shared_from_this (), connection] {
				this_l->close_connection (*connection);
			});
		}
	}

	void close_connection (peer_connection & connection)
	{
		if (!connection.socket.is_open ())
		{
			return;
		}
		boost::system::error_code ec;
		connection.socket.cancel (ec);
		if (ec)
		{
			generator.logger.error (nano::log::type::work_generator, "Error cancelling operation with work peer: {} ({})", connection.endpoint, ec.message ());
			return;
		}
		connection.socket.close (ec);
		if (ec)
		{
			generator.logger.error (nano::log::type::work_generator, "Error closing socket with work peer: {} ({})", connection.endpoint, ec.message ());
		}
	}

	static nano::work_generation_status to_status (outcome outcome_a)
	{
		switch (outcome_a)
		{
			case outcome::success:
				return nano::work_generation_status::success;
			case outcome::cancelled:
				return nano::work_generation_status::cancelled;
			case outcome::local_failure:
			case outcome::peers_failure:
				return nano::work_generation_status::failure;
		}
		debug_assert (false);
		return nano::work_generation_status::failure;
	}

	using http_request = boost::beast::http::request<boost::beast::http::string_body>;

	static std::shared_ptr<http_request> prepare_http_request (nano::tcp_endpoint const & endpoint, std::string body)
	{
		auto result = std::make_shared<http_request> ();
		result->method (boost::beast::http::verb::post);
		result->set (boost::beast::http::field::content_type, "application/json");
		result->set (boost::beast::http::field::host, boost::algorithm::erase_first_copy (endpoint.address ().to_string (), "::ffff:"));
		result->target ("/");
		result->version (11);
		result->body () = std::move (body);
		result->prepare_payload ();
		return result;
	}

	std::string body_generate () const
	{
		boost::property_tree::ptree ptree;
		ptree.put ("action", "work_generate");
		ptree.put ("hash", request.root.to_string ());
		ptree.put ("difficulty", nano::to_string_hex (request.difficulty));
		if (request.account.has_value ())
		{
			ptree.put ("account", request.account.value ().to_account ());
		}
		std::stringstream ostream;
		boost::property_tree::write_json (ostream, ptree);
		return ostream.str ();
	}

	std::string body_cancel () const
	{
		boost::property_tree::ptree ptree;
		ptree.put ("action", "work_cancel");
		ptree.put ("hash", request.root.to_string ());
		std::stringstream ostream;
		boost::property_tree::write_json (ostream, ptree);
		return ostream.str ();
	}

private:
	nano::work_generator & generator;
	bool const use_local;
	std::chrono::steady_clock::time_point const start_time{ std::chrono::steady_clock::now () };

	boost::asio::strand<boost::asio::io_context::executor_type> strand;
	boost::asio::ip::tcp::resolver resolver;

	std::vector<std::shared_ptr<peer_connection>> connections; // Guarded by mutex
	std::vector<std::string> bad_peers; // Guarded by mutex

	std::atomic<size_t> pending; // Outstanding peer endpoints, grows when a hostname resolves to multiple entries
	std::atomic<size_t> failures{ 0 };
	std::atomic<bool> local_started{ false };
	std::atomic<bool> finished{ false };
	std::atomic<bool> aborted{ false };
	nano::mutex mutex;
};

/*
 * work_generator
 */

nano::work_generator::work_generator (nano::node_config const & config_a, nano::network_params const & network_params_a, nano::work_pool & work_pool_a, nano::thread_pool & workers_a, nano::node_observers & observers_a, nano::stats & stats_a, nano::logger & logger_a, boost::asio::io_context & io_ctx_a) :
	config{ config_a },
	network_params{ network_params_a },
	work_pool{ work_pool_a },
	workers{ workers_a },
	observers{ observers_a },
	stats{ stats_a },
	logger{ logger_a },
	io_ctx{ io_ctx_a }
{
}

nano::work_generator::~work_generator ()
{
	stop ();
}

void nano::work_generator::stop ()
{
	if (stopped.exchange (true))
	{
		return;
	}
	std::vector<std::shared_ptr<generation>> to_cancel;
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		for (auto const & [root_l, entry_l] : generations)
		{
			to_cancel.push_back (entry_l.gen);
		}
	}
	for (auto const & generation_l : to_cancel)
	{
		generation_l->cancel ();
	}
}

void nano::work_generator::generate (nano::work_request request, nano::work_callback callback)
{
	stats.inc (nano::stat::type::work_generator, nano::stat::detail::generate);
	submit (request, callback, initial_backoff);
}

void nano::work_generator::generate (nano::work_version version, nano::root const & root, uint64_t difficulty, nano::work_callback callback, std::optional<nano::account> const & account)
{
	generate (nano::work_request{ version, root, difficulty, account, config.work_peers }, std::move (callback));
}

void nano::work_generator::cancel (nano::root const & root)
{
	std::vector<std::shared_ptr<generation>> to_cancel;
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		auto [begin, end] = generations.equal_range (root);
		for (auto it = begin; it != end; ++it)
		{
			to_cancel.push_back (it->second.gen);
		}
	}
	for (auto const & generation_l : to_cancel)
	{
		generation_l->cancel ();
	}
}

void nano::work_generator::submit (nano::work_request const & request, nano::work_callback const & callback, std::chrono::seconds backoff)
{
	if (stopped || !enabled (request.peers))
	{
		stats.inc (nano::stat::type::work_generator, nano::stat::detail::refused);
		if (callback)
		{
			callback (std::nullopt);
		}
		return;
	}
	// Peers known to be unresponsive are raced against the local work pool right away
	bool use_local = (request.peers.empty () || unresponsive_peers) && local_enabled ();
	auto generation_l = std::make_shared<generation> (*this, request, use_local);
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		generations.emplace (request.root, entry{ generation_l, callback, backoff });
	}
	generation_l->start ();
}

void nano::work_generator::finished (generation const & generation_a, nano::work_generation_result result, bool retry_a)
{
	nano::work_callback callback;
	std::chrono::seconds backoff{ initial_backoff };
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		auto [begin, end] = generations.equal_range (generation_a.request.root);
		for (auto it = begin; it != end; ++it)
		{
			if (it->second.gen.get () == &generation_a)
			{
				callback = std::move (it->second.callback);
				backoff = it->second.backoff;
				generations.erase (it);
				break;
			}
		}
	}
	switch (result.status)
	{
		case nano::work_generation_status::success:
		{
			stats.inc (nano::stat::type::work_generator, nano::stat::detail::success);
			logger.info (nano::log::type::work_generator, "Work generation for {}, with a threshold difficulty of {} (multiplier {}x) complete: {} ms",
			result.request.root,
			nano::to_string_hex (result.request.difficulty),
			nano::to_string (nano::difficulty::to_multiplier (result.request.difficulty, network_params.work.threshold_base (result.request.version)), 2),
			result.duration.count ());

			if (callback)
			{
				callback (result.work);
			}
		}
		break;
		case nano::work_generation_status::cancelled:
		{
			stats.inc (nano::stat::type::work_generator, nano::stat::detail::cancelled);
			logger.info (nano::log::type::work_generator, "Work generation for {} was cancelled after {} ms",
			result.request.root,
			result.duration.count ());

			if (callback)
			{
				callback (std::nullopt);
			}
		}
		break;
		case nano::work_generation_status::failure:
		{
			if (retry_a)
			{
				stats.inc (nano::stat::type::work_generator, nano::stat::detail::failure_peers);
				retry (result.request, callback, backoff);
			}
			else
			{
				stats.inc (nano::stat::type::work_generator, nano::stat::detail::failure_local);
				if (callback)
				{
					callback (std::nullopt);
				}
			}
		}
		break;
	}
	observers.work_generation.notify (result);
}

void nano::work_generator::retry (nano::work_request const & request, nano::work_callback const & callback, std::chrono::seconds backoff)
{
	stats.inc (nano::stat::type::work_generator, nano::stat::detail::retry);
	logger.info (nano::log::type::work_generator, "Work peer(s) failed to generate work for root {}, retrying... (backoff: {}s)",
	request.root,
	backoff.count ());

	auto next_backoff = std::min (backoff * 2, max_backoff);
	// Guarded by the stopped flag inside submit; the delayed task queue is drained before this component is destroyed
	workers.post_delayed (backoff, [this, request, callback, next_backoff] {
		submit (request, callback, next_backoff);
	});
}

bool nano::work_generator::local_enabled () const
{
	return config.work_threads > 0 || work_pool.opencl;
}

bool nano::work_generator::enabled () const
{
	return enabled (config.work_peers);
}

bool nano::work_generator::enabled (std::vector<nano::work_peer> const & peers) const
{
	return !peers.empty () || local_enabled ();
}

size_t nano::work_generator::size () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return generations.size ();
}

nano::container_info nano::work_generator::container_info () const
{
	nano::container_info info;
	info.put ("generations", size ());
	return info;
}
