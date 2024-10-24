#pragma once

#include <boost/functional/hash.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <array>
#include <compare>
#include <ostream>

#include <fmt/ostream.h>

namespace nano
{
using uint128_t = boost::multiprecision::uint128_t;
using uint256_t = boost::multiprecision::uint256_t;
using uint512_t = boost::multiprecision::uint512_t;

// SI dividers
nano::uint128_t const Knano_ratio = nano::uint128_t ("1000000000000000000000000000000000"); // 10^33 = 1000 nano
nano::uint128_t const nano_ratio = nano::uint128_t ("1000000000000000000000000000000"); // 10^30 = 1 nano
nano::uint128_t const raw_ratio = nano::uint128_t ("1"); // 10^0

class uint128_union
{
public:
	uint128_union () = default;
	uint128_union (uint64_t);
	uint128_union (nano::uint128_t const &);

	/**
	 * Decode from hex string
	 * @warning Aborts at runtime if the input is invalid
	 */
	explicit uint128_union (std::string const &);

	void encode_hex (std::string &) const;
	bool decode_hex (std::string const &);
	void encode_dec (std::string &) const;
	bool decode_dec (std::string const &, bool = false);
	bool decode_dec (std::string const &, nano::uint128_t);

	std::string format_balance (nano::uint128_t scale, int precision, bool group_digits) const;
	std::string format_balance (nano::uint128_t scale, int precision, bool group_digits, std::locale const & locale) const;

	nano::uint128_t number () const;
	void clear ();
	bool is_zero () const;

	std::string to_string () const;
	std::string to_string_dec () const;

public:
	union
	{
		std::array<uint8_t, 16> bytes;
		std::array<char, 16> chars;
		std::array<uint32_t, 4> dwords;
		std::array<uint64_t, 2> qwords;
	};

public: // Keep operators inlined
	std::strong_ordering operator<=> (nano::uint128_union const & other) const
	{
		return std::memcmp (bytes.data (), other.bytes.data (), 16) <=> 0;
	};
	bool operator== (nano::uint128_union const & other) const
	{
		return *this <=> other == 0;
	}
	operator nano::uint128_t () const
	{
		return number ();
	}
	uint128_union const & as_union () const
	{
		return *this;
	}
};
static_assert (std::is_nothrow_move_constructible<uint128_union>::value, "uint128_union should be noexcept MoveConstructible");

// Balances are 128 bit.
class amount : public uint128_union
{
public:
	using uint128_union::uint128_union;

	auto operator<=> (nano::amount const & other) const
	{
		return uint128_union::operator<=> (other);
	}
	operator nano::uint128_t () const
	{
		return number ();
	}
};

class raw_key;

class uint256_union
{
public:
	uint256_union () = default;
	uint256_union (uint64_t);
	uint256_union (uint256_t const &);

	/**
	 * Decode from hex string
	 * @warning Aborts at runtime if the input is invalid
	 */
	explicit uint256_union (std::string const &);

	void encrypt (nano::raw_key const &, nano::raw_key const &, uint128_union const &);

	uint256_union & operator^= (uint256_union const &);
	uint256_union operator^ (uint256_union const &) const;

	void encode_hex (std::string &) const;
	bool decode_hex (std::string const &);
	void encode_dec (std::string &) const;
	bool decode_dec (std::string const &);

	nano::uint256_t number () const;
	void clear ();
	bool is_zero () const;

	std::string to_string () const;

public:
	union
	{
		std::array<uint8_t, 32> bytes;
		std::array<char, 32> chars;
		std::array<uint32_t, 8> dwords;
		std::array<uint64_t, 4> qwords;
		std::array<uint128_union, 2> owords;
	};

public: // Keep operators inlined
	std::strong_ordering operator<=> (nano::uint256_union const & other) const
	{
		return std::memcmp (bytes.data (), other.bytes.data (), 32) <=> 0;
	};
	bool operator== (nano::uint256_union const & other) const
	{
		return *this <=> other == 0;
	}
	operator nano::uint256_t () const
	{
		return number ();
	}
	uint256_union const & as_union () const
	{
		return *this;
	}
};
static_assert (std::is_nothrow_move_constructible<uint256_union>::value, "uint256_union should be noexcept MoveConstructible");

class link;
class root;
class hash_or_account;

// All keys and hashes are 256 bit.
class block_hash final : public uint256_union
{
public:
	using uint256_union::uint256_union;

public: // Keep operators inlined
	auto operator<=> (nano::block_hash const & other) const
	{
		return uint256_union::operator<=> (other);
	}
	bool operator== (nano::block_hash const & other) const
	{
		return *this <=> other == 0;
	}
	operator nano::uint256_t () const
	{
		return number ();
	}
	operator nano::link () const
	{
		return nano::link{ *this };
	}
	operator nano::root () const
	{
		return nano::root{ *this };
	}
	operator nano::hash_or_account () const
	{
		return nano::hash_or_account{ *this };
	}
};

class public_key final : public uint256_union
{
public:
	using uint256_union::uint256_union;

	public_key ();

	static const public_key & null ();

	std::string to_node_id () const;
	bool decode_node_id (std::string const & source_a);
	void encode_account (std::string &) const;
	std::string to_account () const;
	bool decode_account (std::string const &);

	operator nano::link const & () const;
	operator nano::root const & () const;
	operator nano::hash_or_account const & () const;

public: // Keep operators inlined
	auto operator<=> (nano::public_key const & other) const
	{
		return uint256_union::operator<=> (other);
	}
	bool operator== (nano::public_key const & other) const
	{
		return *this <=> other == 0;
	}
	bool operator== (std::nullptr_t) const
	{
		return *this == null ();
	}
	operator nano::uint256_t () const
	{
		return number ();
	}
};

class wallet_id : public uint256_union
{
	using uint256_union::uint256_union;
};

// These are synonymous
using account = public_key;

class hash_or_account
{
public:
	hash_or_account ();
	hash_or_account (uint64_t);
	explicit hash_or_account (uint256_union const &);

	bool is_zero () const;
	void clear ();

	std::string to_string () const;
	bool decode_hex (std::string const &);
	bool decode_account (std::string const &);
	std::string to_account () const;

public:
	union
	{
		std::array<uint8_t, 32> bytes;
		nano::uint256_union raw; // This can be used when you don't want to explicitly mention either of the types
		nano::account account;
		nano::block_hash hash;
	};

public: // Keep operators inlined
	auto operator<=> (nano::hash_or_account const & other) const
	{
		return raw <=> other.raw;
	};
	bool operator== (nano::hash_or_account const & other) const
	{
		return *this <=> other == 0;
	}
	operator nano::uint256_t () const
	{
		return raw.number ();
	}
	operator nano::uint256_union () const
	{
		return raw;
	}
	nano::account const & as_account () const
	{
		return account;
	}
	nano::block_hash const & as_block_hash () const
	{
		return hash;
	}
	nano::uint256_union const & as_union () const
	{
		return raw;
	}
};

// A link can either be a destination account or source hash
class link final : public hash_or_account
{
public:
	using hash_or_account::hash_or_account;

public: // Keep operators inlined
	auto operator<=> (nano::link const & other) const
	{
		return hash_or_account::operator<=> (other);
	}
	bool operator== (nano::link const & other) const
	{
		return *this <=> other == 0;
	}
};

// A root can either be an open block hash or a previous hash
class root final : public hash_or_account
{
public:
	using hash_or_account::hash_or_account;

	nano::block_hash const & previous () const
	{
		return hash;
	}

public: // Keep operators inlined
	auto operator<=> (nano::root const & other) const
	{
		return hash_or_account::operator<=> (other);
	}
	bool operator== (nano::root const & other) const
	{
		return *this <=> other == 0;
	}
};

// The seed or private key
class raw_key final : public uint256_union
{
public:
	using uint256_union::uint256_union;
	~raw_key ();
	void decrypt (nano::uint256_union const &, nano::raw_key const &, uint128_union const &);
};

class uint512_union
{
public:
	uint512_union () = default;
	uint512_union (nano::uint256_union const &, nano::uint256_union const &);
	uint512_union (nano::uint512_t const &);

	nano::uint512_union & operator^= (nano::uint512_union const &);

	void encode_hex (std::string &) const;
	bool decode_hex (std::string const &);

	void clear ();
	bool is_zero () const;

	nano::uint512_t number () const;

	std::string to_string () const;

public:
	union
	{
		std::array<uint8_t, 64> bytes;
		std::array<uint32_t, 16> dwords;
		std::array<uint64_t, 8> qwords;
		std::array<uint256_union, 2> uint256s;
	};

public: // Keep operators inlined
	std::strong_ordering operator<=> (nano::uint512_union const & other) const
	{
		return std::memcmp (bytes.data (), other.bytes.data (), 64) <=> 0;
	};
	bool operator== (nano::uint512_union const & other) const
	{
		return *this <=> other == 0;
	}
	operator nano::uint512_t () const
	{
		return number ();
	}
	uint512_union const & as_union () const
	{
		return *this;
	}
};
static_assert (std::is_nothrow_move_constructible<uint512_union>::value, "uint512_union should be noexcept MoveConstructible");

class signature : public uint512_union
{
public:
	using uint512_union::uint512_union;
};

class qualified_root : public uint512_union
{
public:
	using uint512_union::uint512_union;

	nano::root root () const
	{
		return nano::root{ uint256s[0] };
	}
	nano::block_hash previous () const
	{
		return nano::block_hash{ uint256s[1] };
	}
};

nano::signature sign_message (nano::raw_key const &, nano::public_key const &, nano::uint256_union const &);
nano::signature sign_message (nano::raw_key const &, nano::public_key const &, uint8_t const *, size_t);
bool validate_message (nano::public_key const &, nano::uint256_union const &, nano::signature const &);
bool validate_message (nano::public_key const &, uint8_t const *, size_t, nano::signature const &);
nano::raw_key deterministic_key (nano::raw_key const &, uint32_t);
nano::public_key pub_key (nano::raw_key const &);

/* Conversion methods */
std::string to_string_hex (uint64_t const);
std::string to_string_hex (uint16_t const);
bool from_string_hex (std::string const &, uint64_t &);

/* Printing adapters */
std::ostream & operator<< (std::ostream &, const uint128_union &);
std::ostream & operator<< (std::ostream &, const uint256_union &);
std::ostream & operator<< (std::ostream &, const uint512_union &);

/**
 * Convert a double to string in fixed format
 * @param precision_a (optional) use a specific precision (default is the maximum)
 */
std::string to_string (double const, int const precision_a = std::numeric_limits<double>::digits10);

namespace difficulty
{
	uint64_t from_multiplier (double const, uint64_t const);
	double to_multiplier (uint64_t const, uint64_t const);
}
}

/*
 * Hashing
 */

namespace std
{
template <>
struct hash<::nano::uint256_union>
{
	size_t operator() (::nano::uint256_union const & data_a) const
	{
		return data_a.qwords[0] + data_a.qwords[1] + data_a.qwords[2] + data_a.qwords[3];
	}
};
template <>
struct hash<::nano::account>
{
	size_t operator() (::nano::account const & data_a) const
	{
		return hash<::nano::uint256_union> () (data_a);
	}
};
template <>
struct hash<::nano::block_hash>
{
	size_t operator() (::nano::block_hash const & data_a) const
	{
		return hash<::nano::uint256_union> () (data_a);
	}
};
template <>
struct hash<::nano::hash_or_account>
{
	size_t operator() (::nano::hash_or_account const & data_a) const
	{
		return hash<::nano::block_hash> () (data_a.as_block_hash ());
	}
};
template <>
struct hash<::nano::raw_key>
{
	size_t operator() (::nano::raw_key const & data_a) const
	{
		return hash<::nano::uint256_union> () (data_a);
	}
};
template <>
struct hash<::nano::root>
{
	size_t operator() (::nano::root const & data_a) const
	{
		return hash<::nano::uint256_union> () (data_a);
	}
};
template <>
struct hash<::nano::wallet_id>
{
	size_t operator() (::nano::wallet_id const & data_a) const
	{
		return hash<::nano::uint256_union> () (data_a);
	}
};
template <>
struct hash<::nano::uint256_t>
{
	size_t operator() (::nano::uint256_t const & number_a) const
	{
		return number_a.convert_to<size_t> ();
	}
};
template <>
struct hash<::nano::uint512_union>
{
	size_t operator() (::nano::uint512_union const & data_a) const
	{
		return hash<::nano::uint256_union> () (data_a.uint256s[0]) + hash<::nano::uint256_union> () (data_a.uint256s[1]);
	}
};
template <>
struct hash<::nano::qualified_root>
{
	size_t operator() (::nano::qualified_root const & data_a) const
	{
		return hash<::nano::uint512_union> () (data_a);
	}
};
}

namespace boost
{
template <>
struct hash<std::reference_wrapper<::nano::block_hash const>>
{
	size_t operator() (std::reference_wrapper<::nano::block_hash const> const & hash_a) const
	{
		std::hash<::nano::block_hash> hash;
		return hash (hash_a);
	}
};
template <>
struct hash<::nano::root>
{
	size_t operator() (::nano::root const & value_a) const
	{
		return std::hash<::nano::root> () (value_a);
	}
};
}

/*
 * Formatters
 */

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
struct fmt::formatter<nano::block_hash> : fmt::formatter<nano::uint256_union>
{
};

template <>
struct fmt::formatter<nano::public_key> : fmt::formatter<nano::uint256_union>
{
};

template <>
struct fmt::formatter<nano::qualified_root> : fmt::formatter<nano::uint512_union>
{
};