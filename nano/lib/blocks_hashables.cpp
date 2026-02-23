#include <nano/crypto/blake2/blake2.h>
#include <nano/lib/assert.hpp>
#include <nano/lib/blocks_hashables.hpp>

void nano::send_hashables::hash (blake2b_state & state) const
{
	blake2b_update (&state, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&state, destination.bytes.data (), sizeof (destination.bytes));
	blake2b_update (&state, balance.bytes.data (), sizeof (balance.bytes));
}

void nano::receive_hashables::hash (blake2b_state & state) const
{
	blake2b_update (&state, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&state, source.bytes.data (), sizeof (source.bytes));
}

void nano::open_hashables::hash (blake2b_state & state) const
{
	blake2b_update (&state, source.bytes.data (), sizeof (source.bytes));
	blake2b_update (&state, representative.bytes.data (), sizeof (representative.bytes));
	blake2b_update (&state, account.bytes.data (), sizeof (account.bytes));
}

void nano::change_hashables::hash (blake2b_state & state) const
{
	blake2b_update (&state, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&state, representative.bytes.data (), sizeof (representative.bytes));
}

void nano::state_hashables::hash (blake2b_state & state) const
{
	blake2b_update (&state, account.bytes.data (), sizeof (account.bytes));
	blake2b_update (&state, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&state, representative.bytes.data (), sizeof (representative.bytes));
	blake2b_update (&state, balance.bytes.data (), sizeof (balance.bytes));
	blake2b_update (&state, link.bytes.data (), sizeof (link.bytes));
}
