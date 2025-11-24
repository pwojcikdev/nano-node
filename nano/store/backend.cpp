#include <nano/store/backend.hpp>

namespace nano::store
{
auto backend::meta () -> result<backend_meta>
{
	try
	{
		open (store::open_mode::read_only, { tables::meta });
	}
	catch (nano::error & e)
	{
		return outcome::failure (e);
	}
}
}