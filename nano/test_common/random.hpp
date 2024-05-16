#pragma once

#include <nano/lib/blocks.hpp>
#include <nano/secure/common.hpp>

namespace nano::test
{
/*
 * Random generators
 */
nano::hash_or_account random_hash_or_account ();
nano::block_hash random_hash ();
nano::account random_account ();
nano::qualified_root random_qualified_root ();
nano::amount random_amount ();
std::shared_ptr<nano::block> random_block ();
}