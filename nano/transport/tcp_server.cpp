#include <nano/messages/messages.hpp>
#include <nano/transport/tcp/handshake.hpp>
#include <nano/transport/tcp_server.hpp>

#include <memory>

nano::transport::tcp_server::tcp_server (asio::io_context & io_ctx_a, nano::network_constants const & network_constants_a, nano::network_filter & network_filter_a, nano::block_uniquer & block_uniquer_a, nano::vote_uniquer & vote_uniquer_a, nano::transport::handshake_provider & handshake_a, nano::stats & stats_a, nano::logger & logger_a, disable_realtime_callback disable_realtime_a, create_channel_callback create_channel_a, message_put_callback message_put_a, std::shared_ptr<nano::transport::tcp_socket> socket_a) :
	io_ctx{ io_ctx_a },
	network_constants{ network_constants_a },
	network_filter{ network_filter_a },
	block_uniquer{ block_uniquer_a },
	vote_uniquer{ vote_uniquer_a },
	handshake{ handshake_a },
	stats{ stats_a },
	logger{ logger_a },
	disable_realtime{ std::move (disable_realtime_a) },
	create_channel{ std::move (create_channel_a) },
	message_put{ std::move (message_put_a) },
	socket{ socket_a },
	strand{ io_ctx_a.get_executor () },
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
		// Context must be running to gracefully stop async tasks
		debug_assert (!io_ctx.stopped ());
		// Ensure that we are not trying to await the task while running on the same thread / io_context
		debug_assert (!io_ctx.get_executor ().running_in_this_thread ());

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
		if (handshake_result == handshake_status::realtime)
		{
			co_await run_realtime ();
		}
		else
		{
			stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_abort);
			logger.debug (nano::log::type::tcp_server, "Handshake aborted: {}", get_remote_endpoint ());
		}
	}
	catch (boost::system::system_error const & ex)
	{
		stats.inc (nano::stat::type::tcp_server_error, nano::to_stat_detail (ex.code ()), nano::stat::dir::in);
		logger.debug (nano::log::type::tcp_server, "Server stopped due to error: {} ({})", ex.code (), get_remote_endpoint ());
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

auto nano::transport::tcp_server::perform_handshake () -> asio::awaitable<handshake_status>
{
	debug_assert (strand.running_in_this_thread ());
	debug_assert (get_type () == nano::transport::socket_type::undefined);
	if (disable_realtime ())
	{
		stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_error);
		logger.debug (nano::log::type::tcp_server, "Handshake attempted with disabled realtime mode ({})", get_remote_endpoint ());
		co_return handshake_status::abort;
	}

	auto const connection_type = socket->get_endpoint_type () == nano::transport::socket_endpoint::client ? nano::transport::connection_type::outbound : nano::transport::connection_type::inbound;
	nano::transport::tcp::handshake_service handshaker{ network_constants, handshake };

	nano::transport::tcp::handshake_service::read_callback const read = [socket_l = socket] (nano::shared_buffer buffer, std::size_t size) -> asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> {
		co_return co_await socket_l->co_read (buffer, size);
	};
	nano::transport::tcp::handshake_service::write_callback const write = [socket_l = socket] (nano::shared_const_buffer const & buffer) -> asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> {
		co_return co_await socket_l->co_write (buffer, buffer.size ());
	};

	if (connection_type == nano::transport::connection_type::outbound)
	{
		stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_initiate, nano::stat::dir::out);
		logger.debug (nano::log::type::tcp_server, "Initiating handshake query ({})", get_remote_endpoint ());
	}

	std::optional<nano::account> node_id;
	try
	{
		node_id = co_await handshaker.run (get_remote_endpoint (), connection_type, read, write,
		[this] (nano::messages::node_id_handshake const &) {
			stats.inc (nano::stat::type::tcp_server, nano::stat::detail::node_id_handshake, nano::stat::dir::in);
			stats.inc (nano::stat::type::tcp_server_message, nano::stat::detail::node_id_handshake, nano::stat::dir::in);
		});
	}
	catch (boost::system::system_error const & ex)
	{
		stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_network_error);
		logger.debug (nano::log::type::tcp_server, "Handshake network error: {} ({})", ex.code ().message (), get_remote_endpoint ());
		co_return handshake_status::abort;
	}

	if (!node_id)
	{
		stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_error);
		logger.debug (nano::log::type::tcp_server, "Invalid handshake message received ({})", get_remote_endpoint ());
		co_return handshake_status::abort;
	}

	bool success = to_realtime_connection (*node_id);
	if (!success)
	{
		stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_error);
		logger.debug (nano::log::type::tcp_server, "Error switching to realtime mode ({})", get_remote_endpoint ());
		co_return handshake_status::abort;
	}

	stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake, nano::stat::dir::in);
	co_return handshake_status::realtime;
}

auto nano::transport::tcp_server::run_realtime () -> asio::awaitable<void>
{
	debug_assert (strand.running_in_this_thread ());
	debug_assert (get_type () == nano::transport::socket_type::realtime);

	logger.debug (nano::log::type::tcp_server, "Running realtime connection: {}", get_remote_endpoint ());

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
				bool added = message_put (std::move (message), channel);
				stats.inc (nano::stat::type::tcp_server, added ? nano::stat::detail::message_queued : nano::stat::detail::message_dropped);
			}
			else
			{
				stats.inc (nano::stat::type::tcp_server, nano::stat::detail::message_ignored);
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
					stats.inc (nano::stat::type::filter, nano::stat::detail::duplicate_publish_message);
				}
				break;
				case nano::deserialize_message_status::duplicate_confirm_ack_message:
				{
					stats.inc (nano::stat::type::filter, nano::stat::detail::duplicate_confirm_ack_message);
				}
				break;
				default:
				{
					logger.debug (nano::log::type::tcp_server, "Error deserializing message: {} ({})",
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
		stats.inc (nano::stat::type::tcp_server_message, to_stat_detail (message->type ()), nano::stat::dir::in);
	}
	else
	{
		stats.inc (nano::stat::type::tcp_server_message_error, to_stat_detail (status), nano::stat::dir::in);
	}

	co_return result;
}

auto nano::transport::tcp_server::receive_message_impl () -> asio::awaitable<nano::deserialize_message_result>
{
	debug_assert (strand.running_in_this_thread ());

	stats.inc (nano::stat::type::tcp_server, nano::stat::detail::read_header, nano::stat::dir::in);
	stats.inc (nano::stat::type::tcp_server_read, nano::stat::detail::header, nano::stat::dir::in);

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
	if (header.network != network_constants.current_network)
	{
		co_return nano::deserialize_message_result{ nullptr, nano::deserialize_message_status::invalid_network };
	}
	if (header.version_using < network_constants.protocol_version_min)
	{
		co_return nano::deserialize_message_result{ nullptr, nano::deserialize_message_status::outdated_version };
	}

	auto const payload_size = header.payload_length_bytes ();

	stats.inc (nano::stat::type::tcp_server, nano::stat::detail::read_payload, nano::stat::dir::in);
	stats.inc (nano::stat::type::tcp_server_read, to_stat_detail (header.type), nano::stat::dir::in);

	auto payload_buffer = payload_size > 0 ? co_await read_socket (payload_size) : nano::buffer_view{ buffer->data (), 0 };

	auto result = nano::deserialize_message (payload_buffer, header,
	network_constants,
	&network_filter,
	&block_uniquer,
	&vote_uniquer);

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

bool nano::transport::tcp_server::to_realtime_connection (nano::account const & node_id)
{
	if (disable_realtime ())
	{
		return false;
	}
	if (socket->type () != nano::transport::socket_type::undefined)
	{
		return false;
	}

	auto channel_l = create_channel (socket, shared_from_this (), node_id);
	if (!channel_l)
	{
		return false;
	}
	channel = channel_l;

	socket->type_set (nano::transport::socket_type::realtime);

	logger.debug (nano::log::type::tcp_server, "Switched to realtime mode ({})", get_remote_endpoint ());

	return true;
}
