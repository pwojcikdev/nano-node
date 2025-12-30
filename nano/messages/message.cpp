#include <nano/lib/object_stream.hpp>
#include <nano/lib/stream.hpp>
#include <nano/messages/message.hpp>

nano::messages::message::message (nano::network_constants const & constants, message_type type_a) :
	header (constants, type_a)
{
}

nano::messages::message::message (message_header const & header_a) :
	header (header_a)
{
}

std::shared_ptr<std::vector<uint8_t>> nano::messages::message::to_bytes () const
{
	auto bytes = std::make_shared<std::vector<uint8_t>> ();
	nano::vectorstream stream (*bytes);
	serialize (stream);
	return bytes;
}

nano::shared_const_buffer nano::messages::message::to_shared_const_buffer () const
{
	return nano::shared_const_buffer (to_bytes ());
}

nano::messages::message_type nano::messages::message::type () const
{
	return header.type;
}

void nano::messages::message::operator() (nano::object_stream & obs) const
{
	obs.write ("header", header);
}
