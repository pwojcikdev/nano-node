#include <nano/crypto_lib/random_pool.hpp>
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

std::shared_ptr<nano::block> nano::test::random_block ()
{
	nano::keypair key;
	auto block = std::make_shared<nano::state_block> (
	nano::test::random_account (),
	nano::test::random_hash (),
	nano::test::random_account (),
	nano::test::random_amount (),
	nano::test::random_hash (),
	key.prv,
	key.pub,
	0);
	return block;
}