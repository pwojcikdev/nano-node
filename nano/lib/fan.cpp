#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/assert.hpp>
#include <nano/lib/fan.hpp>

namespace nano
{
fan::fan (nano::raw_key const & key, std::size_t count_a)
{
	auto first (std::make_unique<nano::raw_key> (key));
	for (auto i (1); i < count_a; ++i)
	{
		auto entry (std::make_unique<nano::raw_key> ());
		nano::random_pool::generate_block (entry->bytes.data (), entry->bytes.size ());
		*first ^= *entry;
		values.push_back (std::move (entry));
	}
	values.push_back (std::move (first));
}

void fan::value (nano::raw_key & prv_a) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	value_get (prv_a);
}

void fan::value_get (nano::raw_key & prv_a) const
{
	debug_assert (!mutex.try_lock ());
	prv_a.clear ();
	for (auto & i : values)
	{
		prv_a ^= *i;
	}
}

void fan::value_set (nano::raw_key const & value_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	nano::raw_key value_l;
	value_get (value_l);
	*(values[0]) ^= value_l;
	*(values[0]) ^= value_a;
}
}
