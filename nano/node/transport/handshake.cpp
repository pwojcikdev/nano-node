#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/transport/handshake.hpp>
#include <nano/secure/common.hpp>

nano::node_handshake::node_handshake (nano::network_params const & network_params_a, nano::keypair const & node_id_a, capabilities_query get_capabilities_a, std::size_t max_peers_per_ip, nano::stats & stats_a, nano::logger & logger_a) :
	network_params{ network_params_a },
	node_id{ node_id_a },
	get_capabilities{ std::move (get_capabilities_a) },
	stats{ stats_a },
	logger{ logger_a },
	cookies{ max_peers_per_ip, logger_a }
{
}

std::optional<nano::messages::node_id_handshake::query_payload> nano::node_handshake::prepare_query (nano::endpoint const & remote_endpoint)
{
	if (auto cookie_l = cookies.assign (remote_endpoint); cookie_l)
	{
		return nano::messages::node_id_handshake::query_payload{ *cookie_l };
	}

	return std::nullopt;
}

nano::messages::node_id_handshake::response_payload nano::node_handshake::prepare_response (nano::messages::node_id_handshake::query_payload const & query, nano::messages::handshake_version version, nano::endpoint const &)
{
	using handshake_version = nano::messages::handshake_version;
	using response_payload = nano::messages::node_id_handshake::response_payload;

	response_payload response{};
	response.node_id = node_id.pub;

	switch (version)
	{
		case handshake_version::v3:
		{
			response_payload::v3_payload payload{};
			payload.salt = nano::random_pool::generate<nano::uint256_union> ();
			payload.genesis = network_params.ledger.genesis->hash ();
			payload.flags = get_capabilities ();
			response.ext = payload;
			break;
		}
		case handshake_version::v2:
		{
			response_payload::v2_payload payload{};
			payload.salt = nano::random_pool::generate<nano::uint256_union> ();
			payload.genesis = network_params.ledger.genesis->hash ();
			response.ext = payload;
			break;
		}
		case handshake_version::v1:
			break;
	}

	response.sign (query.cookie, node_id);
	return response;
}

bool nano::node_handshake::verify_response (nano::messages::node_id_handshake::response_payload const & response, nano::endpoint const & remote_endpoint)
{
	if (response.node_id == node_id.pub)
	{
		stats.inc (nano::stat::type::handshake, nano::stat::detail::invalid_node_id);
		return false;
	}

	if (auto genesis = response.genesis (); genesis && *genesis != network_params.ledger.genesis->hash ())
	{
		stats.inc (nano::stat::type::handshake, nano::stat::detail::invalid_genesis);
		return false;
	}

	auto cookie_l = cookies.cookie (remote_endpoint);
	if (!cookie_l)
	{
		stats.inc (nano::stat::type::handshake, nano::stat::detail::missing_cookie);
		return false;
	}

	if (!response.validate (*cookie_l))
	{
		stats.inc (nano::stat::type::handshake, nano::stat::detail::invalid_signature);
		return false;
	}

	stats.inc (nano::stat::type::handshake, nano::stat::detail::ok);
	return true;
}

void nano::node_handshake::purge (std::chrono::steady_clock::time_point const & cutoff)
{
	cookies.purge (cutoff);
}

std::optional<nano::uint256_union> nano::node_handshake::assign (nano::endpoint const & endpoint)
{
	return cookies.assign (endpoint);
}

bool nano::node_handshake::validate (nano::endpoint const & endpoint, nano::account const & account, nano::signature const & signature)
{
	return cookies.validate (endpoint, account, signature);
}

std::optional<nano::uint256_union> nano::node_handshake::cookie (nano::endpoint const & endpoint)
{
	return cookies.cookie (endpoint);
}

std::size_t nano::node_handshake::cookies_size () const
{
	return cookies.cookies_size ();
}

nano::container_info nano::node_handshake::container_info () const
{
	return cookies.container_info ();
}

nano::syn_cookies::syn_cookies (std::size_t max_cookies_per_ip_a, nano::logger & logger_a) :
	logger{ logger_a },
	max_cookies_per_ip{ max_cookies_per_ip_a }
{
}

std::optional<nano::uint256_union> nano::syn_cookies::assign (nano::endpoint const & endpoint_a)
{
	auto ip_addr (endpoint_a.address ());
	debug_assert (ip_addr.is_v6 ());

	nano::lock_guard<nano::mutex> lock{ syn_cookie_mutex };
	unsigned & ip_cookies = cookies_per_ip[ip_addr];
	std::optional<nano::uint256_union> result;

	if (ip_cookies < max_cookies_per_ip && cookies.find (endpoint_a) == cookies.end ())
	{
		nano::uint256_union query;
		nano::random_pool::generate_block (query.bytes.data (), query.bytes.size ());
		cookies[endpoint_a] = syn_cookie_info{ query, std::chrono::steady_clock::now () };
		++ip_cookies;
		result = query;
	}

	return result;
}

bool nano::syn_cookies::validate (nano::endpoint const & endpoint_a, nano::account const & node_id, nano::signature const & sig)
{
	auto ip_addr (endpoint_a.address ());
	debug_assert (ip_addr.is_v6 ());

	nano::lock_guard<nano::mutex> lock{ syn_cookie_mutex };
	auto result = true;
	auto cookie_it = cookies.find (endpoint_a);

	if (cookie_it != cookies.end () && !nano::validate_message (node_id, cookie_it->second.cookie, sig))
	{
		result = false;
		cookies.erase (cookie_it);

		unsigned & ip_cookies = cookies_per_ip[ip_addr];
		if (ip_cookies > 0)
		{
			--ip_cookies;
		}
		else
		{
			debug_assert (false && "More SYN cookies deleted than created for IP");
		}
	}

	return result;
}

void nano::syn_cookies::purge (std::chrono::steady_clock::time_point const & cutoff_a)
{
	nano::lock_guard<nano::mutex> lock{ syn_cookie_mutex };
	for (auto it = cookies.begin (); it != cookies.end ();)
	{
		if (it->second.created_at < cutoff_a)
		{
			unsigned & per_ip = cookies_per_ip[it->first.address ()];
			if (per_ip > 0)
			{
				--per_ip;
			}
			else
			{
				debug_assert (false && "More SYN cookies deleted than created for IP");
			}

			it = cookies.erase (it);
		}
		else
		{
			++it;
		}
	}
}

std::optional<nano::uint256_union> nano::syn_cookies::cookie (nano::endpoint const & endpoint_a)
{
	auto ip_addr (endpoint_a.address ());
	debug_assert (ip_addr.is_v6 ());

	nano::lock_guard<nano::mutex> lock{ syn_cookie_mutex };
	auto cookie_it = cookies.find (endpoint_a);
	if (cookie_it == cookies.end ())
	{
		return std::nullopt;
	}

	auto cookie_l = cookie_it->second.cookie;
	cookies.erase (cookie_it);

	unsigned & ip_cookies = cookies_per_ip[ip_addr];
	if (ip_cookies > 0)
	{
		--ip_cookies;
	}
	else
	{
		debug_assert (false && "More SYN cookies deleted than created for IP");
	}

	return cookie_l;
}

std::size_t nano::syn_cookies::cookies_size () const
{
	nano::lock_guard<nano::mutex> lock{ syn_cookie_mutex };
	return cookies.size ();
}

nano::container_info nano::syn_cookies::container_info () const
{
	nano::lock_guard<nano::mutex> lock{ syn_cookie_mutex };

	nano::container_info info;
	info.put ("syn_cookies", cookies.size ());
	info.put ("syn_cookies_per_ip", cookies_per_ip.size ());
	return info;
}
