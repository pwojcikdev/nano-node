#pragma once

#include <nano/lib/container_info.hpp>
#include <nano/lib/endpoint.hpp>
#include <nano/lib/fwd.hpp>
#include <nano/lib/keypair.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/node_capabilities.hpp>
#include <nano/messages/node_id_handshake.hpp>
#include <nano/node/fwd.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <unordered_map>

#include <transport/tcp/handshake.hpp>

namespace nano
{
class syn_cookies final
{
public:
	syn_cookies (std::size_t max_peers_per_ip, nano::logger &);

	void purge (std::chrono::steady_clock::time_point const &);
	std::optional<nano::uint256_union> assign (nano::endpoint const &);
	bool validate (nano::endpoint const &, nano::account const &, nano::signature const &);
	std::optional<nano::uint256_union> cookie (nano::endpoint const &);
	std::size_t cookies_size () const;
	nano::container_info container_info () const;

private:
	nano::logger & logger;

	class syn_cookie_info final
	{
	public:
		nano::uint256_union cookie;
		std::chrono::steady_clock::time_point created_at;
	};

	mutable nano::mutex syn_cookie_mutex;
	std::unordered_map<nano::endpoint, syn_cookie_info> cookies;
	std::unordered_map<boost::asio::ip::address, unsigned> cookies_per_ip;
	std::size_t max_cookies_per_ip;
};

class node_handshake final : public nano::transport::tcp::handshake_provider
{
public:
	using capabilities_query = std::function<nano::node_capabilities_flags ()>;

	node_handshake (nano::network_params const &, nano::keypair const &, capabilities_query, std::size_t max_peers_per_ip, nano::stats &, nano::logger &);

	std::optional<nano::messages::node_id_handshake::query_payload> prepare_query (nano::endpoint const & remote_endpoint) override;
	nano::messages::node_id_handshake::response_payload prepare_response (nano::messages::node_id_handshake::query_payload const & query, nano::messages::handshake_version version, nano::endpoint const & remote_endpoint) override;
	bool verify_response (nano::messages::node_id_handshake::response_payload const & response, nano::endpoint const & remote_endpoint) override;

	void purge (std::chrono::steady_clock::time_point const & cutoff);
	std::optional<nano::uint256_union> assign (nano::endpoint const & endpoint);
	bool validate (nano::endpoint const & endpoint, nano::account const & node_id, nano::signature const & signature);
	std::optional<nano::uint256_union> cookie (nano::endpoint const & endpoint);
	std::size_t cookies_size () const;
	nano::container_info container_info () const;

private:
	nano::network_params const & network_params;
	nano::keypair const & node_id;
	capabilities_query get_capabilities;
	nano::stats & stats;
	nano::logger & logger;
	nano::syn_cookies cookies;
};
}
