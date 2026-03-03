#include <nano/lib/block_type.hpp>
#include <nano/lib/blocks_raw.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/ipc/flatbuffers_util.hpp>

std::unique_ptr<nanoapi::BlockStateT> nano::ipc::flatbuffers_builder::from (nano::raw_state_block const & block_a, nano::amount const & amount_a, bool is_state_send_a, bool is_state_epoch_a)
{
	auto block (std::make_unique<nanoapi::BlockStateT> ());
	block->account = block_a.account_field ().to_account ();
	block->hash = block_a.hash ().to_string ();
	block->previous = block_a.previous_field ().to_string ();
	block->representative = block_a.representative_field ().to_account ();
	block->balance = block_a.balance_field ().to_string_dec ();
	block->link = block_a.link_field ().to_string ();
	block->link_as_account = block_a.link_field ().to_account ();
	block->signature = block_a.signature.to_string ();
	block->work = nano::to_string_hex (block_a.work);

	if (is_state_send_a)
	{
		block->subtype = nanoapi::BlockSubType::BlockSubType_send;
	}
	else if (is_state_epoch_a)
	{
		block->subtype = nanoapi::BlockSubType::BlockSubType_epoch;
	}
	else if (block_a.link_field ().is_zero ())
	{
		block->subtype = nanoapi::BlockSubType::BlockSubType_change;
	}
	else
	{
		block->subtype = nanoapi::BlockSubType::BlockSubType_receive;
	}
	return block;
}

std::unique_ptr<nanoapi::BlockSendT> nano::ipc::flatbuffers_builder::from (nano::raw_send_block const & block_a)
{
	auto block (std::make_unique<nanoapi::BlockSendT> ());
	block->hash = block_a.hash ().to_string ();
	block->balance = block_a.balance_field ().to_string_dec ();
	block->destination = block_a.destination_field ().to_account ();
	block->previous = block_a.previous_field ().to_string ();
	block->signature = block_a.signature.to_string ();
	block->work = nano::to_string_hex (block_a.work);
	return block;
}

std::unique_ptr<nanoapi::BlockReceiveT> nano::ipc::flatbuffers_builder::from (nano::raw_receive_block const & block_a)
{
	auto block (std::make_unique<nanoapi::BlockReceiveT> ());
	block->hash = block_a.hash ().to_string ();
	block->source = block_a.source_field ().to_string ();
	block->previous = block_a.previous_field ().to_string ();
	block->signature = block_a.signature.to_string ();
	block->work = nano::to_string_hex (block_a.work);
	return block;
}

std::unique_ptr<nanoapi::BlockOpenT> nano::ipc::flatbuffers_builder::from (nano::raw_open_block const & block_a)
{
	auto block (std::make_unique<nanoapi::BlockOpenT> ());
	block->hash = block_a.hash ().to_string ();
	block->source = block_a.source_field ().to_string ();
	block->account = block_a.account_field ().to_account ();
	block->representative = block_a.representative_field ().to_account ();
	block->signature = block_a.signature.to_string ();
	block->work = nano::to_string_hex (block_a.work);
	return block;
}

std::unique_ptr<nanoapi::BlockChangeT> nano::ipc::flatbuffers_builder::from (nano::raw_change_block const & block_a)
{
	auto block (std::make_unique<nanoapi::BlockChangeT> ());
	block->hash = block_a.hash ().to_string ();
	block->previous = block_a.previous_field ().to_string ();
	block->representative = block_a.representative_field ().to_account ();
	block->signature = block_a.signature.to_string ();
	block->work = nano::to_string_hex (block_a.work);
	return block;
}

nanoapi::BlockUnion nano::ipc::flatbuffers_builder::block_to_union (nano::raw_block const & block_a, nano::amount const & amount_a, bool is_state_send_a, bool is_state_epoch_a)
{
	nanoapi::BlockUnion u;
	switch (block_a.type ())
	{
		case nano::block_type::state:
		{
			u.Set (*from (*block_a.as_state (), amount_a, is_state_send_a, is_state_epoch_a));
			break;
		}
		case nano::block_type::send:
		{
			u.Set (*from (*block_a.as_send ()));
			break;
		}
		case nano::block_type::receive:
		{
			u.Set (*from (*block_a.as_receive ()));
			break;
		}
		case nano::block_type::open:
		{
			u.Set (*from (*block_a.as_open ()));
			break;
		}
		case nano::block_type::change:
		{
			u.Set (*from (*block_a.as_change ()));
			break;
		}

		default:
			debug_assert (false);
	}
	return u;
}
