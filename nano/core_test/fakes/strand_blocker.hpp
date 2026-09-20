#pragma once

#include <nano/lib/async.hpp>

#include <boost/asio/use_future.hpp>

#include <functional>
#include <future>

namespace nano::test
{
/**
 * Occupies a strand until released, so that whatever is posted to the strand meanwhile runs afterwards, in the
 * order it was posted. This is how a test fixes the order in which two events reach code that runs on a strand.
 */
class strand_blocker final
{
public:
	// Returns once the strand is occupied
	explicit strand_blocker (nano::async::strand & strand)
	{
		std::promise<void> running;
		done = asio::post (strand, asio::use_future ([this, &running] () {
			running.set_value ();
			if (auto last = released.get_future ().get ())
			{
				last ();
			}
		}));
		running.get_future ().wait ();
	}

	// Lets the strand go; `last` runs on the strand first, ahead of everything that queued up behind the blocker
	void release (std::function<void ()> last = nullptr)
	{
		released.set_value (std::move (last));
		done.wait ();
	}

private:
	std::promise<std::function<void ()>> released;
	std::future<void> done;
};
}
