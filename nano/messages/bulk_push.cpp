#include <nano/lib/object_stream.hpp>
#include <nano/lib/stream.hpp>
#include <nano/messages/bulk_push.hpp>
#include <nano/messages/message_visitor.hpp>

nano::bulk_push::bulk_push (nano::network_constants const & constants) :
	message (constants, nano::message_type::bulk_push)
{
}

nano::bulk_push::bulk_push (nano::message_header const & header_a) :
	message (header_a)
{
}

bool nano::bulk_push::deserialize (nano::stream & stream_a)
{
	debug_assert (header.type == nano::message_type::bulk_push);
	return false;
}

void nano::bulk_push::serialize (nano::stream & stream_a) const
{
	header.serialize (stream_a);
}

void nano::bulk_push::visit (nano::message_visitor & visitor_a) const
{
	visitor_a.bulk_push (*this);
}

void nano::bulk_push::operator() (nano::object_stream & obs) const
{
	nano::message::operator() (obs); // Write common data
}
