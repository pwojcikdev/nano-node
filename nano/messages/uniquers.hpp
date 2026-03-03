#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/lib/uniquer.hpp>
#include <nano/lib/vote.hpp>

namespace nano
{
using vote_uniquer = nano::uniquer<nano::block_hash, nano::vote>;
}