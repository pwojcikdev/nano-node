#include <nano/lib/stats.hpp>
#include <nano/messages/messages.hpp>
#include <nano/transport/handshake_driver.hpp>
#include <nano/transport/handshake_provider.hpp>
#include <nano/transport/ports.hpp>
#include <nano/transport/tcp_channel.hpp>
#include <nano/transport/tcp_server.hpp>
#include <nano/transport/transport.hpp>
#include <nano/transport/transport_context.hpp>

#include <memory>
#include <variant>

nano::transport::tcp_server::tcp_server (transport_context & ctx_a, std::shared_ptr<tcp_socket> socket_a) :
	ctx{ ctx_a },
	socket{ socket_a },
	strand{ ctx_a.io_ctx.get_executor () },
	task{ strand },
	buffer{ std::make_shared<nano::shared_buffer::element_type> (max_buffer_size) }
{
}

nano::transport::tcp_server::~tcp_server ()
{
	close ();
}

void nano::transport::tcp_server::close ()
{
	stop ();
	socket->close ();
}

void nano::transport::tcp_server::close_async ()
{
	socket->close_async ();
}

// Starting the server must be separate from the constructor to allow the socket to access shared_from_this
void nano::transport::tcp_server::start ()
{
	task = nano::async::task (strand, start_impl ());
}

void nano::transport::tcp_server::stop ()
{
	if (task.running ())
	{
		// Node context must be running to gracefully stop async tasks
		debug_assert (!ctx.io_ctx.stopped ());
		// Ensure that we are not trying to await the task while running on the same thread / io_context
		debug_assert (!ctx.io_ctx.get_executor ().running_in_this_thread ());

		task.cancel ();
		task.join ();
	}
}

auto nano::transport::tcp_server::start_impl () -> asio::awaitable<void>
{
	debug_assert (strand.running_in_this_thread ());
	try
	{
		auto handshake_result = co_await perform_handshake ();

		// Only realtime mode is supported now
		if (handshake_result == handshake_outcome::realtime)
		{
			co_await run_realtime ();
		}
		else
		{
			ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_abort);
			ctx.logger.debug (nano::log::type::tcp_server, "Handshake aborted: {}", get_remote_endpoint ());
		}
	}
	catch (boost::system::system_error const & ex)
	{
		ctx.stats.inc (nano::stat::type::tcp_server_error, nano::to_stat_detail (ex.code ()), nano::stat::dir::in);
		ctx.logger.debug (nano::log::type::tcp_server, "Server stopped due to error: {} ({})", ex.code (), get_remote_endpoint ());
	}
	catch (...)
	{
		release_assert (false, "unexpected exception");
	}
	debug_assert (strand.running_in_this_thread ());

	// Ensure socket gets closed if task is stopped
	close_async ();
}

bool nano::transport::tcp_server::alive () const
{
	return socket->alive ();
}

auto nano::transport::tcp_server::perform_handshake () -> asio::awaitable<handshake_outcome>
{
	debug_assert (strand.running_in_this_thread ());
	debug_assert (get_type () == nano::transport::socket_type::undefined);

	auto role = (socket->get_endpoint_type () == nano::transport::socket_endpoint::client)
	? nano::transport::handshake_role::client
	: nano::transport::handshake_role::server;

	nano::transport::handshake_driver driver{
		*ctx.handshake,
		ctx.network_params.network,
		role,
		get_remote_endpoint (),
		ctx.flags.disable_tcp_realtime
	};

	bool promoted = false;

	// Apply initial events (for client this includes send_request).
	for (auto const & event : driver.begin ())
	{
		bool terminal = co_await apply_handshake_event (event, promoted);
		if (terminal)
		{
			co_return promoted ? handshake_outcome::realtime : handshake_outcome::abort;
		}
	}

	// Pump peer messages through the driver until it reaches a terminal state.
	while (!driver.is_terminal ())
	{
		auto [message, message_status] = co_await receive_message ();
		if (!message)
		{
			ctx.logger.debug (nano::log::type::tcp_server, "Error deserializing handshake message: {} ({})",
			to_string (message_status),
			get_remote_endpoint ());
			co_return handshake_outcome::abort;
		}

		auto * handshake_msg = dynamic_cast<nano::messages::node_id_handshake const *> (message.get ());
		if (!handshake_msg)
		{
			ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_error);
			ctx.logger.debug (nano::log::type::tcp_server, "Non-handshake message received during handshake ({})", get_remote_endpoint ());
			co_return handshake_outcome::abort;
		}

		ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::node_id_handshake, nano::stat::dir::in);
		ctx.logger.debug (nano::log::type::tcp_server, "Handshake message received: {} ({})",
		handshake_msg->query ? (handshake_msg->response ? "query + response" : "query") : (handshake_msg->response ? "response" : "none"),
		get_remote_endpoint ());

		for (auto const & event : driver.on_message (*handshake_msg))
		{
			bool terminal = co_await apply_handshake_event (event, promoted);
			if (terminal)
			{
				co_return promoted ? handshake_outcome::realtime : handshake_outcome::abort;
			}
		}
	}

	// Driver reached terminal without emitting a promote — should be covered above, but
	// keep a fallback for safety.
	ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_failed);
	ctx.logger.debug (nano::log::type::tcp_server, "Failed to complete handshake ({})", get_remote_endpoint ());
	co_return promoted ? handshake_outcome::realtime : handshake_outcome::abort;
}

auto nano::transport::tcp_server::apply_handshake_event (nano::transport::handshake_event const & event, bool & promoted) -> asio::awaitable<bool>
{
	namespace e = nano::transport::handshake_events;

	// Use if-else chains — overloaded lambdas in a coroutine-visit context are awkward.
	if (auto * req = std::get_if<e::send_request> (&event))
	{
		ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_initiate, nano::stat::dir::out);
		ctx.logger.debug (nano::log::type::tcp_server, "Initiating handshake query ({})", get_remote_endpoint ());
		co_await write_handshake_message (req->message);
		ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake, nano::stat::dir::out);
		co_return false;
	}
	if (auto * resp = std::get_if<e::send_response> (&event))
	{
		ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_response, nano::stat::dir::out);
		ctx.logger.debug (nano::log::type::tcp_server, "Responding to handshake ({})", get_remote_endpoint ());
		co_await write_handshake_message (resp->message);
		co_return false;
	}
	if (auto * promote = std::get_if<e::promote_realtime> (&event))
	{
		if (to_realtime_connection (promote->peer_node_id, promote->peer_flags))
		{
			promoted = true;
			co_return true;
		}
		ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_error);
		ctx.logger.debug (nano::log::type::tcp_server, "Error switching to realtime mode ({})", get_remote_endpoint ());
		co_return true;
	}
	if (std::get_if<e::promote_bootstrap> (&event))
	{
		// Legacy bootstrap is no longer supported; driver doesn't emit it today.
		co_return true;
	}
	if (auto * abort_e = std::get_if<e::abort> (&event))
	{
		// Map driver abort reasons to stats where the original code distinguished them.
		if (abort_e->reason == "invalid_response")
		{
			ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_response_invalid);
			ctx.logger.debug (nano::log::type::tcp_server, "Invalid handshake response received ({})", get_remote_endpoint ());
		}
		else
		{
			ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_error);
			ctx.logger.debug (nano::log::type::tcp_server, "Handshake aborted: {} ({})", abort_e->reason, get_remote_endpoint ());
		}
		co_return true;
	}
	// wait_for_message: nothing to do, caller continues the read loop.
	co_return false;
}

auto nano::transport::tcp_server::write_handshake_message (nano::messages::node_id_handshake const & message) -> asio::awaitable<void>
{
	auto shared_const_buffer = message.to_shared_const_buffer ();

	auto [ec, size] = co_await socket->co_write (shared_const_buffer, shared_const_buffer.size ());
	debug_assert (ec || size == shared_const_buffer.size ());
	if (ec)
	{
		ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_network_error);
		ctx.logger.debug (nano::log::type::tcp_server, "Error sending handshake message: {} ({})", ec.message (), get_remote_endpoint ());
		throw boost::system::system_error (ec);
	}
}

auto nano::transport::tcp_server::run_realtime () -> asio::awaitable<void>
{
	debug_assert (strand.running_in_this_thread ());
	debug_assert (get_type () == nano::transport::socket_type::realtime);

	ctx.logger.debug (nano::log::type::tcp_server, "Running realtime connection: {}", get_remote_endpoint ());

	while (!co_await nano::async::cancelled ())
	{
		debug_assert (strand.running_in_this_thread ());

		auto [message, status] = co_await receive_message ();
		if (message)
		{
			realtime_message_visitor realtime_visitor{};
			message->visit (realtime_visitor);

			if (realtime_visitor.process)
			{
				release_assert (channel != nullptr);
				channel->set_last_packet_received (std::chrono::steady_clock::now ());

				// TODO: Throttle if not added
				bool added = ctx.message_sink->on_message (std::move (message), channel);
				ctx.stats.inc (nano::stat::type::tcp_server, added ? nano::stat::detail::message_queued : nano::stat::detail::message_dropped);
			}
			else
			{
				ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::message_ignored);
			}
		}
		else // Error while deserializing message
		{
			debug_assert (status != nano::deserialize_message_status::success);

			switch (status)
			{
				// Avoid too much noise about `duplicate_publish_message` errors
				case nano::deserialize_message_status::duplicate_publish_message:
				{
					ctx.stats.inc (nano::stat::type::filter, nano::stat::detail::duplicate_publish_message);
				}
				break;
				case nano::deserialize_message_status::duplicate_confirm_ack_message:
				{
					ctx.stats.inc (nano::stat::type::filter, nano::stat::detail::duplicate_confirm_ack_message);
				}
				break;
				default:
				{
					ctx.logger.debug (nano::log::type::tcp_server, "Error deserializing message: {} ({})",
					to_string (status),
					get_remote_endpoint ());

					co_return; // Stop receiving further messages
				}
				break;
			}
		}
	}
}

auto nano::transport::tcp_server::receive_message () -> asio::awaitable<nano::deserialize_message_result>
{
	auto result = co_await receive_message_impl ();

	auto const & [message, status] = result;
	if (message)
	{
		ctx.stats.inc (nano::stat::type::tcp_server_message, to_stat_detail (message->type ()), nano::stat::dir::in);
	}
	else
	{
		ctx.stats.inc (nano::stat::type::tcp_server_message_error, to_stat_detail (status), nano::stat::dir::in);
	}

	co_return result;
}

auto nano::transport::tcp_server::receive_message_impl () -> asio::awaitable<nano::deserialize_message_result>
{
	debug_assert (strand.running_in_this_thread ());

	ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::read_header, nano::stat::dir::in);
	ctx.stats.inc (nano::stat::type::tcp_server_read, nano::stat::detail::header, nano::stat::dir::in);

	auto header_payload = co_await read_socket (nano::messages::message_header::size);
	auto header_stream = nano::bufferstream{ header_payload.data (), header_payload.size () };

	bool error = false;
	nano::messages::message_header header{ error, header_stream };

	if (error)
	{
		co_return nano::deserialize_message_result{ nullptr, nano::deserialize_message_status::invalid_header };
	}
	if (!header.is_valid_message_type ())
	{
		co_return nano::deserialize_message_result{ nullptr, nano::deserialize_message_status::invalid_message_type };
	}
	if (header.network != ctx.network_params.network.current_network)
	{
		co_return nano::deserialize_message_result{ nullptr, nano::deserialize_message_status::invalid_network };
	}
	if (header.version_using < ctx.network_params.network.protocol_version_min)
	{
		co_return nano::deserialize_message_result{ nullptr, nano::deserialize_message_status::outdated_version };
	}

	auto const payload_size = header.payload_length_bytes ();

	ctx.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::read_payload, nano::stat::dir::in);
	ctx.stats.inc (nano::stat::type::tcp_server_read, to_stat_detail (header.type), nano::stat::dir::in);

	auto payload_buffer = payload_size > 0 ? co_await read_socket (payload_size) : nano::buffer_view{ buffer->data (), 0 };

	auto result = nano::deserialize_message (payload_buffer, header,
	ctx.network_params.network,
	ctx.network_filter,
	ctx.block_uniquer,
	ctx.vote_uniquer);

	co_return result;
}

auto nano::transport::tcp_server::read_socket (size_t size) const -> asio::awaitable<nano::buffer_view>
{
	debug_assert (strand.running_in_this_thread ());

	auto [ec, size_read] = co_await socket->co_read (buffer, size);
	debug_assert (ec || size_read == size);
	debug_assert (strand.running_in_this_thread ());

	if (ec)
	{
		throw boost::system::system_error (ec);
	}

	release_assert (size_read == size);
	co_return nano::buffer_view{ buffer->data (), size_read };
}

/*
 * realtime_message_visitor
 */

void nano::transport::tcp_server::realtime_message_visitor::keepalive (const nano::messages::keepalive & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::publish (const nano::messages::publish & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::confirm_req (const nano::messages::confirm_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::confirm_ack (const nano::messages::confirm_ack & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::frontier_req (const nano::messages::frontier_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::telemetry_req (const nano::messages::telemetry_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::telemetry_ack (const nano::messages::telemetry_ack & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::asc_pull_req (const nano::messages::asc_pull_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::asc_pull_ack (const nano::messages::asc_pull_ack & message)
{
	process = true;
}

/*
 *
 */

bool nano::transport::tcp_server::to_bootstrap_connection ()
{
	if (ctx.flags.disable_bootstrap_listener)
	{
		return false;
	}
	if (ctx.peer_policy->bootstrap_count () >= ctx.tcp_config.bootstrap_connections_max)
	{
		return false;
	}
	if (socket->type () != nano::transport::socket_type::undefined)
	{
		return false;
	}

	socket->type_set (nano::transport::socket_type::bootstrap);

	ctx.logger.debug (nano::log::type::tcp_server, "Switched to bootstrap mode ({})", get_remote_endpoint ());

	return true;
}

bool nano::transport::tcp_server::to_realtime_connection (nano::account const & node_id, nano::node_capabilities_flags flags)
{
	if (ctx.flags.disable_tcp_realtime)
	{
		return false;
	}
	if (socket->type () != nano::transport::socket_type::undefined)
	{
		return false;
	}

	auto channel_l = ctx.create_channel (socket, shared_from_this (), node_id, flags);
	if (!channel_l)
	{
		return false;
	}
	channel = channel_l;

	socket->type_set (nano::transport::socket_type::realtime);

	ctx.logger.debug (nano::log::type::tcp_server, "Switched to realtime mode ({})", get_remote_endpoint ());

	return true;
}
