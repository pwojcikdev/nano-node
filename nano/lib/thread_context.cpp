#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/thread_context.hpp>

namespace
{
thread_local nano::thread_context::context current_context{};

void set_context (nano::thread_context::context context)
{
	current_context = context;
}
}

nano::thread_context::context nano::thread_context::get ()
{
	return current_context;
}

nano::thread_context::scoped::scoped (nano::thread_context::context context) :
	previous{ nano::thread_context::get () }
{
	set_context (context);
}

nano::thread_context::scoped::scoped (nano::logger & logger, nano::stats & stats) :
	scoped{ nano::thread_context::context{ logger, stats } }
{
}

nano::thread_context::scoped::~scoped ()
{
	set_context (previous);
}

bool nano::thread_context::has_logger ()
{
	return current_context.logger != nullptr;
}

bool nano::thread_context::has_stats ()
{
	return current_context.stats != nullptr;
}

nano::logger & nano::thread_context::logger ()
{
	return current_context.logger ? *current_context.logger : nano::default_logger ();
}

nano::stats & nano::thread_context::stats ()
{
	return current_context.stats ? *current_context.stats : nano::default_stats ();
}
