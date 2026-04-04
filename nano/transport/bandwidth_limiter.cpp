#include <nano/lib/utility.hpp>
#include <nano/transport/bandwidth_limiter.hpp>

/*
 * bandwidth_limiter
 */

nano::transport::bandwidth_limiter::bandwidth_limiter (bandwidth_limiter_config const & config_a) :
	config{ config_a },
	limiter_generic{ config_a.generic_limit, config_a.generic_burst_ratio },
	limiter_bootstrap{ config_a.bootstrap_limit, config_a.bootstrap_burst_ratio }
{
}

nano::rate_limiter & nano::transport::bandwidth_limiter::select_limiter (nano::transport::traffic_type type)
{
	switch (type)
	{
		case nano::transport::traffic_type::bootstrap_server:
			return limiter_bootstrap;
		default:
			return limiter_generic;
	}
}

nano::rate_limiter const & nano::transport::bandwidth_limiter::select_limiter (nano::transport::traffic_type type) const
{
	switch (type)
	{
		case nano::transport::traffic_type::bootstrap_server:
			return limiter_bootstrap;
		default:
			return limiter_generic;
	}
}

bool nano::transport::bandwidth_limiter::should_pass (std::size_t buffer_size, nano::transport::traffic_type type)
{
	auto & limiter = select_limiter (type);
	return limiter.should_pass (buffer_size);
}

void nano::transport::bandwidth_limiter::reset (std::size_t limit, double burst_ratio, nano::transport::traffic_type type)
{
	auto & limiter = select_limiter (type);
	limiter.reset (limit, burst_ratio);
}

nano::container_info nano::transport::bandwidth_limiter::container_info () const
{
	nano::container_info info;
	info.put ("generic", limiter_generic.size ());
	info.put ("bootstrap", limiter_bootstrap.size ());
	return info;
}

std::pair<std::size_t, double> nano::transport::bandwidth_limiter::get_limit (nano::transport::traffic_type type) const
{
	return select_limiter (type).get_limit ();
}
