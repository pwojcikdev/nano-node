#pragma once

#include <nano/lib/rate_limiting.hpp>
#include <nano/transport/traffic_type.hpp>

namespace nano::transport
{
class bandwidth_limiter_config final
{
public:
	std::size_t generic_limit{ 10 * 1024 * 1024 };
	double generic_burst_ratio{ 3.0 };

	std::size_t bootstrap_limit{ 5 * 1024 * 1024 };
	double bootstrap_burst_ratio{ 1.0 };
};

/**
 * Class that tracks and manages bandwidth limits for IO operations
 */
class bandwidth_limiter final
{
public:
	explicit bandwidth_limiter (bandwidth_limiter_config const &);

	/**
	 * Check whether packet falls withing bandwidth limits and should be allowed
	 * @return true if OK, false if needs to be dropped
	 */
	bool should_pass (std::size_t buffer_size, nano::transport::traffic_type type);
	/**
	 * Reset limits of selected limiter type to values passed in arguments
	 */
	void reset (std::size_t limit, double burst_ratio, nano::transport::traffic_type type = nano::transport::traffic_type::generic);

	nano::container_info container_info () const;

	std::pair<std::size_t, double> get_limit (nano::transport::traffic_type type = nano::transport::traffic_type::generic) const;

private:
	/**
	 * Returns reference to limiter corresponding to the limit type
	 */
	nano::rate_limiter & select_limiter (nano::transport::traffic_type type);
	nano::rate_limiter const & select_limiter (nano::transport::traffic_type type) const;

private:
	bandwidth_limiter_config const config;

private:
	nano::rate_limiter limiter_generic;
	nano::rate_limiter limiter_bootstrap;
};
}
