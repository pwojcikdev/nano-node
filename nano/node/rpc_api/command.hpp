#pragma once

#include <nano/node/fwd.hpp>

#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nano::rpc_api
{
// Result type for synchronous commands: holds either a JSON response or an error string
class command_result
{
public:
	static command_result ok (boost::json::object value)
	{
		command_result r;
		r.value_ = std::move (value);
		return r;
	}

	static command_result error (std::string message)
	{
		command_result r;
		r.error_ = std::move (message);
		return r;
	}

	bool is_ok () const
	{
		return value_.has_value ();
	}
	boost::json::object & value ()
	{
		return *value_;
	}
	std::string const & error_message () const
	{
		return *error_;
	}

private:
	std::optional<boost::json::object> value_;
	std::optional<std::string> error_;
};

// Context passed to every command execution.
// Uses shared_ptr for JSON lifetime so async commands can safely capture it.
struct request_context
{
	nano::node & node;
	std::shared_ptr<boost::json::value> json; // Owns the parsed JSON - kept alive for async commands
	std::function<void (boost::json::object)> respond;
	std::function<void (std::string const &)> respond_error;
	std::function<void ()> stop_callback;

	boost::json::object const & params () const
	{
		return json->as_object ();
	}
};

// Base class for all RPC commands
class command
{
public:
	virtual ~command () = default;
	virtual std::string_view name () const = 0;
	virtual bool requires_control () const
	{
		return false;
	}
	virtual void execute (request_context & ctx) = 0;
};

// Convenience base for the common case: synchronous commands that return a result
class sync_command : public command
{
public:
	void execute (request_context & ctx) final
	{
		auto result = handle (ctx);
		if (result.is_ok ())
		{
			ctx.respond (std::move (result.value ()));
		}
		else
		{
			ctx.respond_error (result.error_message ());
		}
	}

	virtual command_result handle (request_context const & ctx) = 0;
};
}
