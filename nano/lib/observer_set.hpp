#pragma once

#include <nano/lib/container_info.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/utility.hpp>

#include <boost/smart_ptr/atomic_shared_ptr.hpp>
#include <boost/smart_ptr/make_shared.hpp>

#include <functional>
#include <vector>

namespace nano
{
/**
 * Observers that are all called on every notification.
 * They are kept in an immutable list that is replaced as a whole when it changes, so a notification only takes a reference to the current list: it copies nothing, holds no lock while observers run and may overlap other notifications.
 * An observer added while a notification is running is first called by the next one.
 */
template <typename... T>
class observer_set final
{
public:
	using observer_type = std::function<void (T const &...)>;

public:
	void add (observer_type observer)
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		auto updated = boost::make_shared<observer_list> (*observers.load ());
		updated->push_back (std::move (observer));
		observers.store (updated);
	}

	void notify (T const &... args) const
	{
		auto const current = observers.load ();
		for (auto const & observer : *current)
		{
			observer (args...);
		}
	}

	bool empty () const
	{
		return observers.load ()->empty ();
	}

	size_t size () const
	{
		return observers.load ()->size ();
	}

	void clear ()
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		observers.store (boost::make_shared<observer_list> ());
	}

	nano::container_info container_info () const
	{
		nano::container_info info;
		info.put ("observers", *observers.load ());
		return info;
	}

private:
	using observer_list = std::vector<observer_type>;

	mutable nano::mutex mutex{ mutex_identifier (mutexes::observer_set) }; // Serializes changes of the list, notifications never take it
	boost::atomic_shared_ptr<observer_list const> observers{ boost::make_shared<observer_list const> () }; // Never null
};

}
