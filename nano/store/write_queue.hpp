#pragma once

#include <nano/lib/locks.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <shared_mutex>

namespace nano::store
{
enum class write_strategy
{
	pessimistic, // Exclusive write lock, no other transactions can read or write
	optimistic, // Shared read lock, other transactions can read but not write
};

/** Distinct areas write locking is done, order is irrelevant */
enum class writer
{
	generic,
	node,
	block_processor,
	confirmation_height,
	pruning,
	voting_final,
	bounded_backlog,
	online_weight,
	testing // Used in tests to emulate a write lock
};

class write_queue;

class write_guard final
{
public:
	explicit write_guard (write_queue & queue, writer type, write_strategy strategy);
	~write_guard ();

	write_guard (write_guard const &) = delete;
	write_guard & operator= (write_guard const &) = delete;
	write_guard (write_guard &&) noexcept;
	write_guard & operator= (write_guard &&) noexcept = delete;

	void release ();
	void renew ();

	bool is_owned () const;

	writer const type;
	write_strategy const strategy;

private:
	write_queue & queue;
	bool owns{ false };
	uint64_t token{ 0 };

	// Lock holders - only one will be active based on strategy
	std::unique_ptr<std::shared_lock<std::shared_mutex>> shared_lock;
	std::unique_ptr<std::unique_lock<std::shared_mutex>> exclusive_lock;
};

/**
 * Allocates database write access in a fair maner rather than directly waiting for mutex aquisition
 * Users should wait() for access to database write transaction and hold the write_guard until complete
 */
class write_queue final
{
	friend class write_guard;

public:
	explicit write_queue ();

	/** Blocks until we are at the head of the queue and blocks other waiters until write_guard goes out of scope */
	[[nodiscard ("write_guard blocks other waiters")]] write_guard wait (writer writer, write_strategy strategy = write_strategy::pessimistic);

	/** Returns true if this writer is anywhere in the queue. Currently only used in tests */
	bool contains (writer writer) const;

	/** Doesn't actually pop anything until the returned write_guard is out of scope */
	void pop ();

private:
	[[nodiscard]] uint64_t acquire (writer writer);
	void release (uint64_t token);

private:
	uint64_t next{ 1 };
	using entry = std::pair<writer, uint64_t>; // uint64_t is a unique id for each write_guard
	std::deque<entry> queue;
	mutable nano::mutex mutex;
	nano::condition_variable condition;

	// Shared mutex for database access control
	mutable std::shared_mutex db_mutex;

	std::function<void ()> guard_finish_callback;

	friend class write_guard; // Allow write_guard to access db_mutex
};
}
