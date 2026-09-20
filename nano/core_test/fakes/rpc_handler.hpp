#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/rpc_handler_interface.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace nano::test
{
/** What a backend was asked to process */
struct rpc_handler_call
{
	int rpc_version{ 0 };
	std::string action; // Version 1 only
	std::string body;
	nano::rpc_handler_request_params params; // Version 2 only
};

/** Echoes every request body back and records the call, so the server side can be tested without any RPC API */
class echo_rpc_handler final : public nano::rpc_handler_interface
{
public:
	void process_request (std::string const & action, std::string const & body, std::function<void (std::string const &)> response) override
	{
		record ({ 1, action, body, {} });
		response (body);
	}

	void process_request_v2 (nano::rpc_handler_request_params const & params, std::string const & body, std::function<void (std::shared_ptr<std::string> const &)> response) override
	{
		record ({ 2, {}, body, params });
		response (std::make_shared<std::string> (body));
	}

	void stop () override
	{
	}

	void rpc_instance (nano::rpc_server &) override
	{
	}

	// Every call so far, oldest first
	std::deque<rpc_handler_call> calls () const
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return recorded;
	}

	std::atomic<int> requests{ 0 };

private:
	void record (rpc_handler_call call)
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		recorded.push_back (std::move (call));
		++requests;
	}

	mutable nano::mutex mutex;
	std::deque<rpc_handler_call> recorded;
};

/** Holds every request until the test responds to it, to keep requests in flight for as long as a test needs */
class deferred_rpc_handler final : public nano::rpc_handler_interface
{
public:
	void process_request (std::string const &, std::string const &, std::function<void (std::string const &)> response) override
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		pending.push_back (std::move (response));
	}

	void process_request_v2 (nano::rpc_handler_request_params const &, std::string const &, std::function<void (std::shared_ptr<std::string> const &)> response) override
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		pending.push_back ([response = std::move (response)] (std::string const & body) {
			response (std::make_shared<std::string> (body));
		});
	}

	void stop () override
	{
	}

	void rpc_instance (nano::rpc_server &) override
	{
	}

	// Requests received and not responded to yet
	std::size_t pending_count () const
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return pending.size ();
	}

	// Responds to every held request from the calling thread
	void respond_all (std::string const & body)
	{
		decltype (pending) released;
		{
			nano::lock_guard<nano::mutex> lock{ mutex };
			released.swap (pending);
		}
		for (auto const & response : released)
		{
			response (body);
		}
	}

private:
	mutable nano::mutex mutex;
	std::deque<std::function<void (std::string const &)>> pending;
};

/** Throws from every call, as a backend with a bug would */
class throwing_rpc_handler final : public nano::rpc_handler_interface
{
public:
	void process_request (std::string const &, std::string const &, std::function<void (std::string const &)>) override
	{
		++requests;
		throw std::logic_error ("backend failure");
	}

	void process_request_v2 (nano::rpc_handler_request_params const &, std::string const &, std::function<void (std::shared_ptr<std::string> const &)>) override
	{
		++requests;
		throw std::logic_error ("backend failure");
	}

	void stop () override
	{
	}

	void rpc_instance (nano::rpc_server &) override
	{
	}

	std::atomic<int> requests{ 0 };
};

/** Takes every request and lets go of it without ever responding */
class dropping_rpc_handler final : public nano::rpc_handler_interface
{
public:
	void process_request (std::string const &, std::string const &, std::function<void (std::string const &)>) override
	{
		++requests;
	}

	void process_request_v2 (nano::rpc_handler_request_params const &, std::string const &, std::function<void (std::shared_ptr<std::string> const &)>) override
	{
		++requests;
	}

	void stop () override
	{
	}

	void rpc_instance (nano::rpc_server &) override
	{
	}

	std::atomic<int> requests{ 0 };
};
}
