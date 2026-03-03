#pragma once

#include <nano/ipc_flatbuffers_lib/generated/flatbuffers/nanoapi_generated.h>

#include <memory>

namespace nano
{
class amount;
class raw_block;
struct raw_state_block;
struct raw_send_block;
struct raw_receive_block;
struct raw_open_block;
struct raw_change_block;
namespace ipc
{
	/**
	 * Utilities to convert between blocks and Flatbuffers equivalents
	 */
	class flatbuffers_builder
	{
	public:
		static nanoapi::BlockUnion block_to_union (nano::raw_block const & block_a, nano::amount const & amount_a, bool is_state_send_a = false, bool is_state_epoch_a = false);
		static std::unique_ptr<nanoapi::BlockStateT> from (nano::raw_state_block const & block_a, nano::amount const & amount_a, bool is_state_send_a, bool is_state_epoch_a);
		static std::unique_ptr<nanoapi::BlockSendT> from (nano::raw_send_block const & block_a);
		static std::unique_ptr<nanoapi::BlockReceiveT> from (nano::raw_receive_block const & block_a);
		static std::unique_ptr<nanoapi::BlockOpenT> from (nano::raw_open_block const & block_a);
		static std::unique_ptr<nanoapi::BlockChangeT> from (nano::raw_change_block const & block_a);
	};
}
}
