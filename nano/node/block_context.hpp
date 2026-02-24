#pragma once

#include <nano/lib/blocks_raw.hpp>
#include <nano/lib/stored_block.hpp>
#include <nano/node/block_source.hpp>
#include <nano/secure/common.hpp>

#include <future>
#include <optional>

namespace nano
{
class block_context
{
public:
	using result_t = nano::block_status;
	using callback_t = std::function<void (result_t)>;

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

	// Legacy constructor (converts via to_raw)
	block_context (std::shared_ptr<nano::block> const & block, nano::block_source source, callback_t callback = nullptr) :
		input{ nano::to_raw (*block) },
		source{ source },
		callback{ std::move (callback) }
	{
		debug_assert (source != nano::block_source::unknown);
	}

	std::future<result_t> get_future ()
	{
		return promise.get_future ();
	}

	void set_result (result_t result)
	{
		promise.set_value (result);
	}

private:
	std::promise<result_t> promise;
};
}
