#pragma once

#include <nano/lib/blocks_raw.hpp>
#include <nano/lib/stored_block.hpp>
#include <nano/node/block_source.hpp>
#include <nano/secure/common.hpp>

#include <future>
#include <optional>

namespace nano
{
struct block_result
{
	nano::block_status status{ nano::block_status::invalid };
	std::optional<nano::stored_block> block; // Set when status == progress
};

class block_context
{
public:
	using callback_t = std::function<void (nano::block_status)>;

public: // Keep fields public for simplicity
	nano::raw_block input;
	std::optional<nano::stored_block> block; // Set on successful processing (progress)
	nano::block_source source;
	callback_t callback;
	std::chrono::steady_clock::time_point arrival{ std::chrono::steady_clock::now () };

public:
	block_context (nano::raw_block input, nano::block_source source, callback_t callback = nullptr) :
		input{ std::move (input) },
		source{ source },
		callback{ std::move (callback) }
	{
		debug_assert (source != nano::block_source::unknown);
	}

	std::future<nano::block_result> get_future ()
	{
		return promise.get_future ();
	}

	void set_result (nano::block_result result)
	{
		promise.set_value (std::move (result));
	}

private:
	std::promise<nano::block_result> promise;
};
}
