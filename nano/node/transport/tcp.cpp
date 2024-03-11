#include <nano/lib/config.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/message_deserializer.hpp>
#include <nano/node/transport/tcp.hpp>

#include <boost/format.hpp>

/*
 * channel_tcp
 */

nano::transport::channel_tcp::channel_tcp (nano::node & node_a, std::weak_ptr<nano::transport::socket> socket_a) :
	channel (node_a),
	socket (std::move (socket_a))
{
}

nano::transport::channel_tcp::~channel_tcp ()
{
	nano::lock_guard<nano::mutex> lk{ channel_mutex };
	// Close socket. Exception: socket is used by tcp_server
	if (auto socket_l = socket.lock ())
	{
		if (!temporary)
		{
			socket_l->close ();
		}
	}
}

std::size_t nano::transport::channel_tcp::hash_code () const
{
	std::hash<::nano::tcp_endpoint> hash;
	return hash (get_tcp_endpoint ());
}

bool nano::transport::channel_tcp::operator== (nano::transport::channel const & other_a) const
{
	bool result (false);
	auto other_l (dynamic_cast<nano::transport::channel_tcp const *> (&other_a));
	if (other_l != nullptr)
	{
		return *this == *other_l;
	}
	return result;
}

void nano::transport::channel_tcp::send_buffer (nano::shared_const_buffer const & buffer_a, std::function<void (boost::system::error_code const &, std::size_t)> const & callback_a, nano::transport::buffer_drop_policy policy_a, nano::transport::traffic_type traffic_type)
{
	if (auto socket_l = socket.lock ())
	{
		if (!socket_l->max (traffic_type) || (policy_a == nano::transport::buffer_drop_policy::no_socket_drop && !socket_l->full (traffic_type)))
		{
			socket_l->async_write (
			buffer_a, [endpoint_a = socket_l->remote_endpoint (), node = std::weak_ptr<nano::node> (node.shared ()), callback_a] (boost::system::error_code const & ec, std::size_t size_a) {
				if (auto node_l = node.lock ())
				{
					if (!ec)
					{
						node_l->network.tcp_channels.update (endpoint_a);
					}
					if (ec == boost::system::errc::host_unreachable)
					{
						node_l->stats.inc (nano::stat::type::error, nano::stat::detail::unreachable_host, nano::stat::dir::out);
					}
					if (callback_a)
					{
						callback_a (ec, size_a);
					}
				}
			},
			traffic_type);
		}
		else
		{
			if (policy_a == nano::transport::buffer_drop_policy::no_socket_drop)
			{
				node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_write_no_socket_drop, nano::stat::dir::out);
			}
			else
			{
				node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_write_drop, nano::stat::dir::out);
			}
			if (callback_a)
			{
				callback_a (boost::system::errc::make_error_code (boost::system::errc::no_buffer_space), 0);
			}
		}
	}
	else if (callback_a)
	{
		node.background ([callback_a] () {
			callback_a (boost::system::errc::make_error_code (boost::system::errc::not_supported), 0);
		});
	}
}

std::string nano::transport::channel_tcp::to_string () const
{
	return nano::util::to_str (get_tcp_endpoint ());
}

void nano::transport::channel_tcp::set_endpoint ()
{
	nano::lock_guard<nano::mutex> lk{ channel_mutex };
	debug_assert (endpoint == nano::tcp_endpoint (boost::asio::ip::address_v6::any (), 0)); // Not initialized endpoint value
	// Calculate TCP socket endpoint
	if (auto socket_l = socket.lock ())
	{
		endpoint = socket_l->remote_endpoint ();
	}
}

void nano::transport::channel_tcp::operator() (nano::object_stream & obs) const
{
	nano::transport::channel::operator() (obs); // Write common data

	obs.write ("socket", socket);
}

/*
 * tcp_channels
 */

nano::transport::tcp_channels::tcp_channels (nano::node & node) :
	node{ node }
{
}

bool nano::transport::tcp_channels::insert (std::shared_ptr<nano::transport::channel_tcp> const & channel_a, std::shared_ptr<nano::transport::socket> const & socket_a, std::shared_ptr<nano::transport::tcp_server> const & server_a)
{
	auto endpoint (channel_a->get_tcp_endpoint ());
	debug_assert (endpoint.address ().is_v6 ());
	auto udp_endpoint (nano::transport::map_tcp_to_endpoint (endpoint));
	bool error (true);
	if (!node.network.not_a_peer (udp_endpoint, node.config.allow_local_peers) && !stopped)
	{
		nano::unique_lock<nano::mutex> lock{ mutex };
		auto existing (channels.get<endpoint_tag> ().find (endpoint));
		if (existing == channels.get<endpoint_tag> ().end ())
		{
			auto node_id (channel_a->get_node_id ());
			if (!channel_a->temporary)
			{
				channels.get<node_id_tag> ().erase (node_id);
			}
			channels.get<endpoint_tag> ().emplace (channel_a, socket_a, server_a);
			attempts.get<endpoint_tag> ().erase (endpoint);
			error = false;
			lock.unlock ();
			node.network.channel_observer (channel_a);
		}
	}
	return error;
}

void nano::transport::tcp_channels::erase (nano::tcp_endpoint const & endpoint_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	channels.get<endpoint_tag> ().erase (endpoint_a);
}

std::size_t nano::transport::tcp_channels::size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return channels.size ();
}

std::shared_ptr<nano::transport::channel_tcp> nano::transport::tcp_channels::find_channel (nano::tcp_endpoint const & endpoint_a) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	std::shared_ptr<nano::transport::channel_tcp> result;
	auto existing (channels.get<endpoint_tag> ().find (endpoint_a));
	if (existing != channels.get<endpoint_tag> ().end ())
	{
		result = existing->channel;
	}
	return result;
}

std::unordered_set<std::shared_ptr<nano::transport::channel>> nano::transport::tcp_channels::random_set (std::size_t count_a, uint8_t min_version, bool include_temporary_channels_a) const
{
	std::unordered_set<std::shared_ptr<nano::transport::channel>> result;
	result.reserve (count_a);
	nano::lock_guard<nano::mutex> lock{ mutex };
	// Stop trying to fill result with random samples after this many attempts
	auto random_cutoff (count_a * 2);
	auto peers_size (channels.size ());
	// Usually count_a will be much smaller than peers.size()
	// Otherwise make sure we have a cutoff on attempting to randomly fill
	if (!channels.empty ())
	{
		for (auto i (0); i < random_cutoff && result.size () < count_a; ++i)
		{
			auto index (nano::random_pool::generate_word32 (0, static_cast<CryptoPP::word32> (peers_size - 1)));

			auto channel = channels.get<random_access_tag> ()[index].channel;
			if (!channel->alive ())
			{
				continue;
			}

			if (channel->get_network_version () >= min_version && (include_temporary_channels_a || !channel->temporary))
			{
				result.insert (channel);
			}
		}
	}
	return result;
}

void nano::transport::tcp_channels::random_fill (std::array<nano::endpoint, 8> & target_a) const
{
	auto peers (random_set (target_a.size ()));
	debug_assert (peers.size () <= target_a.size ());
	auto endpoint (nano::endpoint (boost::asio::ip::address_v6{}, 0));
	debug_assert (endpoint.address ().is_v6 ());
	std::fill (target_a.begin (), target_a.end (), endpoint);
	auto j (target_a.begin ());
	for (auto i (peers.begin ()), n (peers.end ()); i != n; ++i, ++j)
	{
		debug_assert ((*i)->get_endpoint ().address ().is_v6 ());
		debug_assert (j < target_a.end ());
		*j = (*i)->get_endpoint ();
	}
}

bool nano::transport::tcp_channels::store_all (bool clear_peers)
{
	// We can't hold the mutex while starting a write transaction, so
	// we collect endpoints to be saved and then relase the lock.
	std::vector<nano::endpoint> endpoints;
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		endpoints.reserve (channels.size ());
		std::transform (channels.begin (), channels.end (),
		std::back_inserter (endpoints), [] (auto const & channel) { return nano::transport::map_tcp_to_endpoint (channel.endpoint ()); });
	}
	bool result (false);
	if (!endpoints.empty ())
	{
		// Clear all peers then refresh with the current list of peers
		auto transaction (node.store.tx_begin_write ({ tables::peers }));
		if (clear_peers)
		{
			node.store.peer.clear (transaction);
		}
		for (auto const & endpoint : endpoints)
		{
			node.store.peer.put (transaction, nano::endpoint_key{ endpoint.address ().to_v6 ().to_bytes (), endpoint.port () });
		}
		result = true;
	}
	return result;
}

std::shared_ptr<nano::transport::channel_tcp> nano::transport::tcp_channels::find_node_id (nano::account const & node_id_a)
{
	std::shared_ptr<nano::transport::channel_tcp> result;
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto existing (channels.get<node_id_tag> ().find (node_id_a));
	if (existing != channels.get<node_id_tag> ().end ())
	{
		result = existing->channel;
	}
	return result;
}

nano::tcp_endpoint nano::transport::tcp_channels::bootstrap_peer ()
{
	nano::tcp_endpoint result (boost::asio::ip::address_v6::any (), 0);
	nano::lock_guard<nano::mutex> lock{ mutex };
	for (auto i (channels.get<last_bootstrap_attempt_tag> ().begin ()), n (channels.get<last_bootstrap_attempt_tag> ().end ()); i != n;)
	{
		if (i->channel->get_network_version () >= node.network_params.network.protocol_version_min)
		{
			result = nano::transport::map_endpoint_to_tcp (i->channel->get_peering_endpoint ());
			channels.get<last_bootstrap_attempt_tag> ().modify (i, [] (channel_tcp_wrapper & wrapper_a) {
				wrapper_a.channel->set_last_bootstrap_attempt (std::chrono::steady_clock::now ());
			});
			i = n;
		}
		else
		{
			++i;
		}
	}
	return result;
}

void nano::transport::tcp_channels::start ()
{
	ongoing_keepalive ();
	ongoing_merge (0);
}

void nano::transport::tcp_channels::stop ()
{
	stopped = true;
	nano::unique_lock<nano::mutex> lock{ mutex };
	// Close all TCP sockets
	for (auto const & channel : channels)
	{
		if (channel.socket)
		{
			channel.socket->close ();
		}
		// Remove response server
		if (channel.response_server)
		{
			channel.response_server->stop ();
		}
	}
	channels.clear ();
}

bool nano::transport::tcp_channels::max_ip_connections (nano::tcp_endpoint const & endpoint_a)
{
	if (node.flags.disable_max_peers_per_ip)
	{
		return false;
	}
	bool result{ false };
	auto const address (nano::transport::ipv4_address_or_ipv6_subnet (endpoint_a.address ()));
	nano::unique_lock<nano::mutex> lock{ mutex };
	result = channels.get<ip_address_tag> ().count (address) >= node.network_params.network.max_peers_per_ip;
	if (!result)
	{
		result = attempts.get<ip_address_tag> ().count (address) >= node.network_params.network.max_peers_per_ip;
	}
	if (result)
	{
		node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_max_per_ip, nano::stat::dir::out);
	}
	return result;
}

bool nano::transport::tcp_channels::max_subnetwork_connections (nano::tcp_endpoint const & endpoint_a)
{
	if (node.flags.disable_max_peers_per_subnetwork)
	{
		return false;
	}
	bool result{ false };
	auto const subnet (nano::transport::map_address_to_subnetwork (endpoint_a.address ()));
	nano::unique_lock<nano::mutex> lock{ mutex };
	result = channels.get<subnetwork_tag> ().count (subnet) >= node.network_params.network.max_peers_per_subnetwork;
	if (!result)
	{
		result = attempts.get<subnetwork_tag> ().count (subnet) >= node.network_params.network.max_peers_per_subnetwork;
	}
	if (result)
	{
		node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_max_per_subnetwork, nano::stat::dir::out);
	}
	return result;
}

bool nano::transport::tcp_channels::max_ip_or_subnetwork_connections (nano::tcp_endpoint const & endpoint_a)
{
	return max_ip_connections (endpoint_a) || max_subnetwork_connections (endpoint_a);
}

bool nano::transport::tcp_channels::reachout (nano::endpoint const & endpoint_a)
{
	auto tcp_endpoint (nano::transport::map_endpoint_to_tcp (endpoint_a));
	// Don't overload single IP
	bool error = node.network.excluded_peers.check (tcp_endpoint) || max_ip_or_subnetwork_connections (tcp_endpoint);
	if (!error && !node.flags.disable_tcp_realtime)
	{
		// Don't keepalive to nodes that already sent us something
		error |= find_channel (tcp_endpoint) != nullptr;
		nano::lock_guard<nano::mutex> lock{ mutex };
		auto inserted (attempts.emplace (tcp_endpoint));
		error |= !inserted.second;
	}
	return error;
}

std::unique_ptr<nano::container_info_component> nano::transport::tcp_channels::collect_container_info (std::string const & name)
{
	std::size_t channels_count;
	std::size_t attemps_count;
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		channels_count = channels.size ();
		attemps_count = attempts.size ();
	}

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "channels", channels_count, sizeof (decltype (channels)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "attempts", attemps_count, sizeof (decltype (attempts)::value_type) }));

	return composite;
}

void nano::transport::tcp_channels::purge (std::chrono::steady_clock::time_point const & cutoff_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	// Remove channels with dead underlying sockets
	erase_if (channels, [] (auto const & entry) {
		return !entry.channel->alive ();
	});

	auto disconnect_cutoff (channels.get<last_packet_sent_tag> ().lower_bound (cutoff_a));
	channels.get<last_packet_sent_tag> ().erase (channels.get<last_packet_sent_tag> ().begin (), disconnect_cutoff);

	// Remove keepalive attempt tracking for attempts older than cutoff
	auto attempts_cutoff (attempts.get<last_attempt_tag> ().lower_bound (cutoff_a));
	attempts.get<last_attempt_tag> ().erase (attempts.get<last_attempt_tag> ().begin (), attempts_cutoff);

	// Check if any tcp channels belonging to old protocol versions which may still be alive due to async operations
	auto lower_bound = channels.get<version_tag> ().lower_bound (node.network_params.network.protocol_version_min);
	channels.get<version_tag> ().erase (channels.get<version_tag> ().begin (), lower_bound);
}

void nano::transport::tcp_channels::ongoing_keepalive ()
{
	nano::keepalive message{ node.network_params.network };
	node.network.random_fill (message.peers);
	nano::unique_lock<nano::mutex> lock{ mutex };
	// Wake up channels
	std::vector<std::shared_ptr<nano::transport::channel_tcp>> send_list;
	auto keepalive_sent_cutoff (channels.get<last_packet_sent_tag> ().lower_bound (std::chrono::steady_clock::now () - node.network_params.network.keepalive_period));
	for (auto i (channels.get<last_packet_sent_tag> ().begin ()); i != keepalive_sent_cutoff; ++i)
	{
		send_list.push_back (i->channel);
	}
	lock.unlock ();
	for (auto & channel : send_list)
	{
		channel->send (message);
	}
	std::weak_ptr<nano::node> node_w (node.shared ());
	node.workers.add_timed_task (std::chrono::steady_clock::now () + node.network_params.network.keepalive_period, [node_w] () {
		if (auto node_l = node_w.lock ())
		{
			if (!node_l->network.tcp_channels.stopped)
			{
				node_l->network.tcp_channels.ongoing_keepalive ();
			}
		}
	});
}

void nano::transport::tcp_channels::ongoing_merge (size_t channel_index)
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	std::optional<nano::keepalive> keepalive;
	size_t count = 0;
	while (!keepalive && channels.size () > 0 && count++ < channels.size ())
	{
		++channel_index;
		if (channels.size () <= channel_index)
		{
			channel_index = 0;
		}
		auto server = channels.get<random_access_tag> ()[channel_index].response_server;
		if (server && server->last_keepalive)
		{
			keepalive = std::move (server->last_keepalive);
			server->last_keepalive = std::nullopt;
		}
	}
	lock.unlock ();
	if (keepalive)
	{
		ongoing_merge (channel_index, *keepalive, 1);
	}
	else
	{
		std::weak_ptr<nano::node> node_w = node.shared ();
		node.workers.add_timed_task (std::chrono::steady_clock::now () + node.network_params.network.merge_period, [node_w, channel_index] () {
			if (auto node_l = node_w.lock ())
			{
				if (!node_l->network.tcp_channels.stopped)
				{
					node_l->network.tcp_channels.ongoing_merge (channel_index);
				}
			}
		});
	}
}

void nano::transport::tcp_channels::ongoing_merge (size_t channel_index, nano::keepalive keepalive, size_t peer_index)
{
	debug_assert (peer_index < keepalive.peers.size ());
	node.network.merge_peer (keepalive.peers[peer_index++]);
	if (peer_index < keepalive.peers.size ())
	{
		std::weak_ptr<nano::node> node_w = node.shared ();
		node.workers.add_timed_task (std::chrono::steady_clock::now () + node.network_params.network.merge_period, [node_w, channel_index, keepalive, peer_index] () {
			if (auto node_l = node_w.lock ())
			{
				if (!node_l->network.tcp_channels.stopped)
				{
					node_l->network.tcp_channels.ongoing_merge (channel_index, keepalive, peer_index);
				}
			}
		});
	}
	else
	{
		std::weak_ptr<nano::node> node_w = node.shared ();
		node.workers.add_timed_task (std::chrono::steady_clock::now () + node.network_params.network.merge_period, [node_w, channel_index] () {
			if (auto node_l = node_w.lock ())
			{
				if (!node_l->network.tcp_channels.stopped)
				{
					node_l->network.tcp_channels.ongoing_merge (channel_index);
				}
			}
		});
	}
}

void nano::transport::tcp_channels::list (std::deque<std::shared_ptr<nano::transport::channel>> & deque_a, uint8_t minimum_version_a, bool include_temporary_channels_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	// clang-format off
	nano::transform_if (channels.get<random_access_tag> ().begin (), channels.get<random_access_tag> ().end (), std::back_inserter (deque_a),
		[include_temporary_channels_a, minimum_version_a](auto & channel_a) { return channel_a.channel->get_network_version () >= minimum_version_a && (include_temporary_channels_a || !channel_a.channel->temporary); },
		[](auto const & channel) { return channel.channel; });
	// clang-format on
}

void nano::transport::tcp_channels::modify (std::shared_ptr<nano::transport::channel_tcp> const & channel_a, std::function<void (std::shared_ptr<nano::transport::channel_tcp> const &)> modify_callback_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto existing (channels.get<endpoint_tag> ().find (channel_a->get_tcp_endpoint ()));
	if (existing != channels.get<endpoint_tag> ().end ())
	{
		channels.get<endpoint_tag> ().modify (existing, [modify_callback = std::move (modify_callback_a)] (channel_tcp_wrapper & wrapper_a) {
			modify_callback (wrapper_a.channel);
		});
	}
}

void nano::transport::tcp_channels::update (nano::tcp_endpoint const & endpoint_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	auto existing (channels.get<endpoint_tag> ().find (endpoint_a));
	if (existing != channels.get<endpoint_tag> ().end ())
	{
		channels.get<endpoint_tag> ().modify (existing, [] (channel_tcp_wrapper & wrapper_a) {
			wrapper_a.channel->set_last_packet_sent (std::chrono::steady_clock::now ());
		});
	}
}

void nano::transport::tcp_channels::start_tcp (nano::endpoint const & endpoint_a)
{
	auto socket = std::make_shared<nano::transport::socket> (node);
	std::weak_ptr<nano::transport::socket> socket_w (socket);
	std::weak_ptr<nano::node> node_w (node.shared ());

	socket->async_connect (nano::transport::map_endpoint_to_tcp (endpoint_a), [node_w, socket, endpoint_a] (boost::system::error_code const & ec) {
		if (auto node_l = node_w.lock ())
		{
			if (ec)
			{
				// Failed connect
				// TODO: Stat & log
			}
			else
			{
				auto server = std::make_shared<nano::transport::tcp_server> (node_l, socket, false);
				server->start ();
				server->send_handshake_query ();
			}
		}
	});
}