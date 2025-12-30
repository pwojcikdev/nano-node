#include <nano/lib/object_stream.hpp>
#include <nano/lib/stream.hpp>
#include <nano/messages/bulk_push.hpp>
#include <nano/messages/message_visitor.hpp>

nano::messages::bulk_push::bulk_push (nano::network_constants const & constants) :
	message (constants, message_type::bulk_push)
{
}

nano::messages::bulk_push::bulk_push (message_header const & header_a) :
	message (header_a)
{
}

bool nano::messages::bulk_push::deserialize (nano::stream & stream_a)
{
	debug_assert (header.type == message_type::bulk_push);
	return false;
}

void nano::messages::bulk_push::serialize (nano::stream & stream_a) const
{
	header.serialize (stream_a);
}

void nano::messages::bulk_push::visit (message_visitor & visitor_a) const
{
	visitor_a.bulk_push (*this);
}

void nano::messages::bulk_push::operator() (nano::object_stream & obs) const
{
	message::operator() (obs); // Write common data
}
