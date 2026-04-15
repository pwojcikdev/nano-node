#pragma once

#include <nano/lib/fwd.hpp>

namespace nano::thread_context
{
struct context
{
	nano::logger * logger{ nullptr };
	nano::stats * stats{ nullptr };

	context () = default;
	context (nano::logger & logger_a) :
		logger{ &logger_a }
	{
	}
	context (nano::logger & logger_a, nano::stats & stats_a) :
		logger{ &logger_a },
		stats{ &stats_a }
	{
	}

	explicit operator bool () const
	{
		return logger != nullptr || stats != nullptr;
	}
};

context get ();

class scoped final
{
public:
	explicit scoped (context);
	scoped (nano::logger &, nano::stats &);
	~scoped ();

	scoped (scoped const &) = delete;
	scoped (scoped &&) = delete;
	scoped & operator= (scoped const &) = delete;
	scoped & operator= (scoped &&) = delete;

private:
	context previous;
};

bool has_logger ();
bool has_stats ();

nano::logger & logger ();
nano::stats & stats ();
}
