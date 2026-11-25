#include <nano/boost/process/process.hpp>
#include <nano/lib/logging.hpp>
#include <nano/node/node_rpc_config.hpp>
#include <nano/node/rpc_process.hpp>

#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;

nano::rpc_process::rpc_process (nano::node_rpc_config const & config, std::filesystem::path const & data_path, std::string_view network, nano::logger & logger_a, std::vector<std::string> const & config_overrides, boost::asio::io_context & io_ctx, std::function<void ()> stop_owner) :
	logger{ logger_a }
{
	auto const & path = config.child_process.rpc_path;
	if (!std::filesystem::exists (path))
	{
		throw std::runtime_error ("RPC is configured to spawn a new process however the file cannot be found at: " + path);
	}

	std::vector<std::string> args{ "--daemon", "--data_path", data_path.string (), "--network", std::string{ network } };
	if (!config_overrides.empty ())
	{
		args.emplace_back ("--config");
		args.insert (args.end (), config_overrides.begin (), config_overrides.end ());
	}
	child = boost::process::child (
	path, args, boost::process::env[managed_env] = "1", io_ctx, boost::process::on_exit = [stop_owner = std::move (stop_owner)] (int status, std::error_code const & ec) {
		if (!ec && status == node_stop_exit_code)
		{
			stop_owner ();
		}
	});
}

void nano::rpc_process::stop ()
{
	std::error_code ec;

	auto const deadline = std::chrono::steady_clock::now () + grace_period;
	while (child.running (ec) && std::chrono::steady_clock::now () < deadline)
	{
		std::this_thread::sleep_for (100ms);
	}

	if (child.running (ec))
	{
		logger.warn (nano::log::type::rpc_process, "RPC process did not exit on its own, terminating it");
		child.terminate (ec);
	}
	child.wait (ec);
}
