#include <nano/node/rpc_api/registry.hpp>

namespace nano::rpc_api
{
registry & registry::instance ()
{
	static registry inst;
	return inst;
}

void registry::register_command (std::unique_ptr<command> cmd)
{
	auto name = std::string (cmd->name ());
	commands.emplace (std::move (name), std::move (cmd));
}

void registry::register_alias (std::string alias, std::string_view target)
{
	aliases.emplace (std::move (alias), std::string (target));
}

command * registry::find (std::string_view action) const
{
	// Direct lookup
	auto it = commands.find (std::string (action));
	if (it != commands.end ())
		return it->second.get ();
	// Alias lookup
	auto alias_it = aliases.find (std::string (action));
	if (alias_it != aliases.end ())
	{
		auto target_it = commands.find (alias_it->second);
		if (target_it != commands.end ())
			return target_it->second.get ();
	}
	return nullptr;
}

bool registry::requires_control (std::string_view action) const
{
	auto * cmd = find (action);
	return cmd ? cmd->requires_control () : false;
}

std::size_t registry::size () const
{
	return commands.size ();
}
}
