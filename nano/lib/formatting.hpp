#pragma once

#include <nano/lib/common.hpp>
#include <nano/lib/numbers.hpp>

#include <boost/system/error_code.hpp>

#include <concepts>
#include <initializer_list>

#include <fmt/format.h>
#include <fmt/ostream.h>

namespace nano::fmt_support
{
// Helper to declaratively parse named format specifiers into an enum value
template <typename E>
constexpr auto parse_spec (fmt::format_parse_context & ctx, std::initializer_list<std::pair<std::string_view, E>> specs, E & out)
{
	auto it = ctx.begin ();
	auto spec = std::string_view{ it, ctx.end () };
	for (auto const & [name, value] : specs)
	{
		if (spec.starts_with (name))
		{
			out = value;
			return it + name.size ();
		}
	}
	return it;
}

// Concept for enums with an ADL-findable to_string() returning string_view
template <typename T>
concept to_string_formattable_enum = std::is_enum_v<T> && requires (T const & t) {
	{
		to_string (t)
	} -> std::same_as<std::string_view>;
};
}

template <nano::fmt_support::to_string_formattable_enum T>
struct fmt::formatter<T> : fmt::formatter<std::string_view>
{
	auto format (T const & value, format_context & ctx) const
	{
		return fmt::formatter<std::string_view>::format (to_string (value), ctx);
	}
};

template <>
struct fmt::formatter<nano::endpoint> : fmt::ostream_formatter
{
};
template <>
struct fmt::formatter<nano::ip_address> : fmt::ostream_formatter
{
};

template <>
struct fmt::formatter<nano::uint128_t> : fmt::ostream_formatter
{
};
template <>
struct fmt::formatter<nano::uint256_t> : fmt::ostream_formatter
{
};
template <>
struct fmt::formatter<nano::uint512_t> : fmt::ostream_formatter
{
};

template <>
struct fmt::formatter<nano::uint128_union> : fmt::ostream_formatter
{
};
template <>
struct fmt::formatter<nano::uint256_union> : fmt::ostream_formatter
{
};
template <>
struct fmt::formatter<nano::uint512_union> : fmt::ostream_formatter
{
};
template <>
struct fmt::formatter<nano::hash_or_account> : fmt::ostream_formatter
{
};
template <>
struct fmt::formatter<nano::block_hash> : fmt::formatter<nano::uint256_union>
{
};
template <>
struct fmt::formatter<nano::qualified_root> : fmt::formatter<nano::uint512_union>
{
};
template <>
struct fmt::formatter<nano::root> : fmt::formatter<nano::hash_or_account>
{
};
template <>
struct fmt::formatter<nano::wallet_id> : fmt::formatter<nano::uint256_union>
{
};

// Supports format specifiers: {} = hex, {:account} = account (nano_...), {:node_id} = node id (node_...)
template <>
struct fmt::formatter<nano::public_key> : fmt::formatter<std::string>
{
	enum class presentation
	{
		hex,
		account,
		node_id,
	};

	presentation pres = presentation::hex;

	constexpr auto parse (format_parse_context & ctx)
	{
		return nano::fmt_support::parse_spec (ctx,
		{
		{ "account", presentation::account },
		{ "node_id", presentation::node_id },
		},
		pres);
	}

	auto format (nano::public_key const & key, format_context & ctx) const
	{
		switch (pres)
		{
			case presentation::account:
				return fmt::formatter<std::string>::format (key.to_account (), ctx);
			case presentation::node_id:
				return fmt::formatter<std::string>::format (key.to_node_id (), ctx);
			default:
				return fmt::formatter<std::string>::format (key.to_string (), ctx);
		}
	}
};

template <>
struct fmt::formatter<boost::system::error_code> : fmt::formatter<std::string>
{
	auto format (const boost::system::error_code & ec, format_context & ctx)
	{
		return fmt::format_to (ctx.out (), "{} {}:{}", ec.message (), ec.value (), ec.category ().name ());
	}
};