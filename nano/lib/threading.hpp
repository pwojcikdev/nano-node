#pragma once

#include <nano/lib/thread_context.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/utility.hpp>

#include <boost/thread/thread.hpp>

#include <thread>

namespace nano
{
namespace thread_attributes
{
	boost::thread::attributes get_default ();
} // namespace thread_attributes

namespace thread
{
template <typename F>
std::thread create (nano::thread_role::name role, nano::thread_context::context context, F && task)
{
	return std::thread{ [role, context, task = std::forward<F> (task)] () mutable {
		nano::thread_context::scoped thread_context{ context };
		nano::thread_role::set (role);
		task ();
	} };
}

template <typename F>
std::thread create (nano::thread_role::name role, F && task)
{
	return create (role, nano::thread_context::get (), std::forward<F> (task));
}

template <typename F>
boost::thread create (boost::thread::attributes const & attributes, nano::thread_role::name role, nano::thread_context::context context, F && task)
{
	return boost::thread{ attributes, [role, context, task = std::forward<F> (task)] () mutable {
		nano::thread_context::scoped thread_context{ context };
		nano::thread_role::set (role);
		task ();
	} };
}

template <typename F>
boost::thread create (boost::thread::attributes const & attributes, nano::thread_role::name role, F && task)
{
	return create (attributes, role, nano::thread_context::get (), std::forward<F> (task));
}
} // namespace thread

/**
 * Number of available logical processor cores. Might be overridden by setting `NANO_HARDWARE_CONCURRENCY` environment variable
 */
unsigned hardware_concurrency ();

/**
 * If thread is joinable joins it, otherwise does nothing
 * Returns thread.joinable()
 */
bool join_or_pass (std::thread &);
} // namespace nano
