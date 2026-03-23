#pragma once

#include <nano/node/rpc_api/command.hpp>

#include <memory>
#include <string_view>
#include <unordered_map>

namespace nano::rpc_api
{
class registry
{
public:
	static registry & instance ();

	void register_command (std::unique_ptr<command> cmd);
	void register_alias (std::string alias, std::string_view target);

	command * find (std::string_view action) const;
	bool requires_control (std::string_view action) const;
	std::size_t size () const;

private:
	registry () = default;
	std::unordered_map<std::string, std::unique_ptr<command>> commands;
	std::unordered_map<std::string, std::string> aliases;
};

struct registrar
{
	registrar (std::unique_ptr<command> cmd)
	{
		registry::instance ().register_command (std::move (cmd));
	}
};
}

#define REGISTER_RPC_COMMAND(CommandClass)                    \
	static nano::rpc_api::registrar _registrar_##CommandClass \
	{                                                         \
		std::make_unique<CommandClass> ()                     \
	}
