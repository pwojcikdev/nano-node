#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/blockbuilders.hpp>
#include <nano/test_common/random.hpp>

nano::hash_or_account nano::test::random_hash_or_account ()
{
	nano::hash_or_account random_hash;
	nano::random_pool::generate_block (random_hash.bytes.data (), random_hash.bytes.size ());
	return random_hash;
}

nano::block_hash nano::test::random_hash ()
{
	return nano::test::random_hash_or_account ().as_block_hash ();
}

nano::account nano::test::random_account ()
{
	return nano::test::random_hash_or_account ().as_account ();
}

nano::qualified_root nano::test::random_qualified_root ()
{
	return { nano::test::random_hash (), nano::test::random_hash () };
}

nano::amount nano::test::random_amount ()
{
	nano::amount result;
	nano::random_pool::generate_block (result.bytes.data (), result.bytes.size ());
	return result;
}

nano::raw_block nano::test::random_block ()
{
	nano::keypair key;
	nano::block_builder builder;
	return builder
	.state ()
	.account (nano::test::random_account ())
	.previous (nano::test::random_hash ())
	.representative (nano::test::random_account ())
	.balance (nano::test::random_amount ())
	.link (nano::test::random_hash ())
	.sign (key.prv, key.pub)
	.work (0)
	.build ();
}
