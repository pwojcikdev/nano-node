#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/test_common/common.hpp>

nano::logger & nano::test::default_logger ()
{
	static nano::logger logger{ "tests" };
	return logger;
}

nano::stats & nano::test::default_stats ()
{
	static nano::stats stats{ nano::test::default_logger () };
	return stats;
}