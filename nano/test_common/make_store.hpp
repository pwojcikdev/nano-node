#pragma once

#include <memory>

namespace nano::store
{
class ledger_store;
}

namespace nano::test
{
std::unique_ptr<nano::store::ledger_store> make_store ();
}
