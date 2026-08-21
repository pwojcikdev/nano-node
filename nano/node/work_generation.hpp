#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/work_version.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace nano
{
/** Address and port of a remote work peer */
struct work_peer final
{
	std::string address;
	uint16_t port;

	bool operator== (work_peer const &) const = default;
};

/** Parameters of a single work generation request */
struct work_request final
{
	nano::work_version version;
	nano::root root;
	uint64_t difficulty;
	std::optional<nano::account> account{};
	std::vector<nano::work_peer> peers{};
};

enum class work_generation_status
{
	success,
	cancelled,
	failure,
};

/** Outcome of a finished work generation attempt, published to observers */
struct work_generation_result final
{
	nano::work_request request;
	nano::work_generation_status status;
	uint64_t work{ 0 };
	std::string winner{}; // Peer "address:port" or "local"
	std::vector<std::string> bad_peers{}; // Peers that failed or returned invalid work
	std::chrono::milliseconds duration{ 0 };
};

/** Invoked exactly once with the generated work, or std::nullopt on failure or cancellation */
using work_callback = std::function<void (std::optional<uint64_t>)>;
}
