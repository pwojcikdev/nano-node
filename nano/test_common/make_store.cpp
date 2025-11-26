#include <nano/lib/files.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/make_store.hpp>
#include <nano/secure/common.hpp>
#include <nano/store/store.hpp>
#include <nano/test_common/make_store.hpp>

std::unique_ptr<nano::store::ledger_store> nano::test::make_store ()
{
	// Create a simple stats instance for testing
	static nano::stats test_stats{ nano::default_logger () };
	return nano::make_store (nano::default_logger (), test_stats, nano::unique_path (), nano::dev::constants);
}
