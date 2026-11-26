#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/portmapping.hpp>
#include <nano/test_common/system.hpp>

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cstdlib>
#include <cstring>
#include <map>

namespace nano::test
{
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

class port_mapping_fixture : public ::testing::Test
{
protected:
	void SetUp () override
	{
		auto config = system.default_config ();
		config.external_port = 4100;
		node = system.add_node (config);
		auto & mapping = node->port_mapping;
		mapping.address = boost::asio::ip::address_v4::loopback ();
		auto url = "http://127.0.0.1:" + std::to_string (acceptor.local_endpoint ().port ()) + "/control";
		// miniupnpc frees controlURL with free() once the discovered gateway is replaced.
		mapping.upnp.urls.controlURL = static_cast<char *> (std::malloc (url.size () + 1));
		ASSERT_NE (nullptr, mapping.upnp.urls.controlURL);
		std::strcpy (mapping.upnp.urls.controlURL, url.c_str ());
		std::strcpy (mapping.upnp.data.first.servicetype, service);

		set_response ("GetExternalIPAddress", "<NewExternalIPAddress>192.0.2.14</NewExternalIPAddress>");
		accept ();
		server = std::thread{ [this] { io.run (); } };
	}

	void TearDown () override
	{
		io.stop ();
		if (server.joinable ())
		{
			server.join ();
		}
	}

	void set_response (std::string const & action, std::string const & fields)
	{
		std::lock_guard guard{ mutex };
		responses[action] = fields;
	}

	void set_entry (std::string const & client, unsigned port, unsigned duration = 1500)
	{
		set_response ("GetSpecificPortMappingEntry", "<NewInternalClient>" + client + "</NewInternalClient><NewInternalPort>" + std::to_string (port) + "</NewInternalPort><NewEnabled>1</NewEnabled><NewLeaseDuration>" + std::to_string (duration) + "</NewLeaseDuration>");
	}

	std::string last_request (std::string const & action)
	{
		std::lock_guard guard{ mutex };
		return requests[action];
	}

	bool check ()
	{
		return node->port_mapping.check_lost_or_old_mapping ();
	}

	void refresh ()
	{
		node->port_mapping.refresh_mapping ();
	}

	void shutdown ()
	{
		node->port_mapping.shutdown ();
	}

	nano::test::system system;
	std::shared_ptr<nano::node> node;

private:
	void accept ()
	{
		acceptor.async_accept ([this] (boost::system::error_code ec, tcp::socket socket) {
			if (!ec)
			{
				boost::beast::flat_buffer buffer;
				http::request<http::string_body> request;
				http::read (socket, buffer, request);
				auto header = std::string{ request["SOAPAction"] };
				auto action = header.substr (header.find ('#') + 1);
				if (action.ends_with ('"'))
				{
					action.pop_back ();
				}
				http::response<http::string_body> response{ http::status::ok, 11 };
				{
					std::lock_guard guard{ mutex };
					requests[action] = request.body ();
					response.body () = "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:" + action + "Response xmlns:u=\"" + service + "\">" + responses[action] + "</u:" + action + "Response></s:Body></s:Envelope>";
				}
				response.prepare_payload ();
				http::write (socket, response);
				accept ();
			}
		});
	}

	static constexpr auto service = "urn:schemas-upnp-org:service:WANIPConnection:1";
	boost::asio::io_context io;
	tcp::acceptor acceptor{ io, { boost::asio::ip::address_v4::loopback (), 0 } };
	std::thread server;
	std::mutex mutex;
	std::map<std::string, std::string> responses;
	std::map<std::string, std::string> requests;
};

TEST_F (port_mapping_fixture, mapping_must_target_node)
{
	auto const port = node->network.endpoint ().port ();
	set_entry ("127.0.0.1", port);
	ASSERT_FALSE (check ());
	ASSERT_EQ (node->config.external_port, node->port_mapping.external_address ().port ());

	set_entry ("127.0.0.2", port);
	ASSERT_FALSE (check ()); // Leave the other client's active lease alone.
	EXPECT_EQ (0, node->port_mapping.external_address ().port ());
	set_entry ("127.0.0.1", port == 1 ? 2 : port - 1);
	ASSERT_FALSE (check ());
	EXPECT_EQ (0, node->port_mapping.external_address ().port ());
}

TEST_F (port_mapping_fixture, removes_own_wildcard_mapping)
{
	auto const port = node->network.endpoint ().port ();
	set_entry ("127.0.0.1", port, 30);
	ASSERT_TRUE (check ());
	refresh ();
	auto const add = last_request ("AddPortMapping");
	ASSERT_NE (std::string::npos, add.find ("<NewRemoteHost></NewRemoteHost>"));
	ASSERT_NE (std::string::npos, add.find ("<NewInternalPort>" + std::to_string (port) + "</NewInternalPort>"));
	shutdown ();
	auto const removed = last_request ("DeletePortMapping");
	EXPECT_NE (std::string::npos, removed.find ("<NewRemoteHost></NewRemoteHost>"));
	EXPECT_NE (std::string::npos, removed.find ("<NewExternalPort>4100</NewExternalPort>"));
}

TEST_F (port_mapping_fixture, retains_reassigned_mapping_on_shutdown)
{
	auto const port = node->network.endpoint ().port ();
	set_entry ("127.0.0.1", port);
	ASSERT_FALSE (check ());
	ASSERT_EQ (node->config.external_port, node->port_mapping.external_address ().port ());
	refresh ();
	set_entry ("127.0.0.2", port);
	shutdown ();
	EXPECT_TRUE (last_request ("DeletePortMapping").empty ());
}

TEST_F (port_mapping_fixture, preserves_existing_permanent_mapping)
{
	set_entry ("127.0.0.1", node->network.endpoint ().port (), 0);
	EXPECT_FALSE (check ());
	EXPECT_EQ (node->config.external_port, node->port_mapping.external_address ().port ());
	shutdown ();
	EXPECT_TRUE (last_request ("DeletePortMapping").empty ());
}
}
