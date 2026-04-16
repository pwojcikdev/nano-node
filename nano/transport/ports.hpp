#pragma once

#include <nano/messages/fwd.hpp>
#include <nano/transport/fwd.hpp>

#include <memory>

namespace nano::transport
{
/**
 * Port: delivers inbound messages and new channels to the node.
 *
 * This is the typed replacement for `transport_context::on_message` and
 * `transport_context::process_inbound`. `nano::node` supplies a concrete
 * adapter that forwards into `message_processor` / `inbound()`.
 */
class message_sink
{
public:
	virtual ~message_sink () = default;

	/// Async delivery: message is handed off to a queue. Returns true if accepted.
	virtual bool on_message (std::unique_ptr<nano::messages::message>, std::shared_ptr<nano::transport::channel>) = 0;

	/// Synchronous inbound processing — used by the loopback channel to deliver
	/// locally-generated messages back into the node without a queue.
	virtual void process_inbound (nano::messages::message const &, std::shared_ptr<nano::transport::channel>) = 0;
};
}
