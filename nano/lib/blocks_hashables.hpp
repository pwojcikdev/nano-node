#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/lib/numbers.hpp>

#include <cstddef>

typedef struct blake2b_state__ blake2b_state;

namespace nano
{
struct send_hashables
{
	nano::block_hash previous;
	nano::account destination;
	nano::amount balance;

	bool operator== (send_hashables const &) const = default;
	void hash (blake2b_state &) const;

	static std::size_t constexpr size = sizeof (previous) + sizeof (destination) + sizeof (balance);
};

struct receive_hashables
{
	nano::block_hash previous;
	nano::block_hash source;

	bool operator== (receive_hashables const &) const = default;
	void hash (blake2b_state &) const;

	static std::size_t constexpr size = sizeof (previous) + sizeof (source);
};

struct open_hashables
{
	nano::block_hash source;
	nano::account representative;
	nano::account account;

	bool operator== (open_hashables const &) const = default;
	void hash (blake2b_state &) const;

	static std::size_t constexpr size = sizeof (source) + sizeof (representative) + sizeof (account);
};

struct change_hashables
{
	nano::block_hash previous;
	nano::account representative;

	bool operator== (change_hashables const &) const = default;
	void hash (blake2b_state &) const;

	static std::size_t constexpr size = sizeof (previous) + sizeof (representative);
};

struct state_hashables
{
	nano::account account;
	nano::block_hash previous;
	nano::account representative;
	nano::amount balance;
	nano::link link;

	bool operator== (state_hashables const &) const = default;
	void hash (blake2b_state &) const;

	static std::size_t constexpr size = sizeof (account) + sizeof (previous) + sizeof (representative) + sizeof (balance) + sizeof (link);
};
}
