#pragma once

#include <nano/node/transport/channel.hpp>

#include <fmt/format.h>

#include <concepts>

template <typename T>
concept channel_derived = std::derived_from<T, nano::transport::channel>;

template <>
struct fmt::formatter<nano::transport::channel> : fmt::formatter<std::string>
{
	auto format (nano::transport::channel const & channel, format_context & ctx) const
	{
		return fmt::formatter<std::string>::format (channel.to_string (), ctx);
	}
};

template <channel_derived T>
struct fmt::formatter<std::shared_ptr<T>> : fmt::formatter<std::string>
{
	auto format (std::shared_ptr<T> const & channel, format_context & ctx) const
	{
		if (channel)
		{
			return fmt::formatter<std::string>::format (channel->to_string (), ctx);
		}
		else
		{
			return fmt::formatter<std::string>::format ("<null>", ctx);
		}
	}
};
