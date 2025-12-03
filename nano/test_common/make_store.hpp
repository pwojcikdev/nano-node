#pragma once

#include <nano/store/fwd.hpp>

#include <filesystem>
#include <memory>

/**
 * Helper functions to create a backend directly for testing
 */
namespace nano::test
{
std::unique_ptr<nano::store::backend> make_backend (std::filesystem::path path = {});
std::unique_ptr<nano::store::ledger_store> make_store (std::filesystem::path path = {});
}
