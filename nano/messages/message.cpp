#include <nano/lib/object_stream.hpp>
#include <nano/lib/stream.hpp>
#include <nano/messages/message.hpp>

nano::message::message (nano::network_constants const & constants, nano::message_type type_a) :
	header (constants, type_a)
{
}

nano::message::message (nano::message_header const & header_a) :
	header (header_a)
{
}

std::shared_ptr<std::vector<uint8_t>> nano::message::to_bytes () const
{
	auto bytes = std::make_shared<std::vector<uint8_t>> ();
	nano::vectorstream stream (*bytes);
	serialize (stream);
	return bytes;
}

nano::shared_const_buffer nano::message::to_shared_const_buffer () const
{
	return shared_const_buffer (to_bytes ());
}

nano::message_type nano::message::type () const
{
	return header.type;
}

void nano::message::operator() (nano::object_stream & obs) const
{
	obs.write ("header", header);
}
