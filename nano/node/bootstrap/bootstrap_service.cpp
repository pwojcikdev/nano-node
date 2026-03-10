#include <nano/lib/blocks.hpp>
#include <nano/node/bootstrap/bootstrap_service.hpp>
#include <nano/node/ledger_notifications.hpp>
#include <nano/secure/ledger.hpp>

nano::bootstrap_service::bootstrap_service (nano::node_config const & node_config_a, nano::ledger & ledger_a, nano::ledger_notifications & ledger_notifications_a,
nano::block_processor & block_processor_a, nano::network & network_a, nano::stats & stat_a, nano::logger & logger_a) :
	ctx{ std::make_unique<nano::bootstrap::bootstrap_context> (node_config_a, ledger_a, block_processor_a, network_a, stat_a, logger_a) },
	ledger_notifications{ ledger_notifications_a }
{
	// Inspect all processed blocks
	ledger_notifications.blocks_processed.add ([this] (auto const & batch) {
		{
			nano::lock_guard<nano::mutex> lock{ ctx->mutex };

			auto transaction = ctx->ledger.tx_begin_read ();
			for (auto const & [result, block_context] : batch)
			{
				debug_assert (block_context.block != nullptr);
				ctx->inspect (transaction, result, *block_context.block, block_context.source);
			}
		}
		ctx->condition.notify_all ();
	});

	// Unblock rolled back accounts as the dependency is no longer valid
	ledger_notifications.blocks_rolled_back.add ([this] (auto const & blocks, auto const & rollback_root) {
		nano::lock_guard<nano::mutex> lock{ ctx->mutex };
		for (auto const & block : blocks)
		{
			debug_assert (block != nullptr);
			ctx->accounts.unblock (block->account ());
		}
	});
}

nano::bootstrap_service::~bootstrap_service ()
{
}

void nano::bootstrap_service::start ()
{
	ctx->start ();
}

void nano::bootstrap_service::stop ()
{
	ctx->stop ();
}

void nano::bootstrap_service::process (nano::messages::asc_pull_ack const & message, std::shared_ptr<nano::transport::channel> const & channel)
{
	ctx->process (message, channel);
}

void nano::bootstrap_service::reset ()
{
	ctx->reset ();
}

void nano::bootstrap_service::prioritize (nano::account const & account)
{
	nano::lock_guard<nano::mutex> lock{ ctx->mutex };
	ctx->accounts.priority_set (account);
}

std::size_t nano::bootstrap_service::priority_size () const
{
	nano::lock_guard<nano::mutex> lock{ ctx->mutex };
	return ctx->accounts.priority_size ();
}

std::size_t nano::bootstrap_service::blocked_size () const
{
	nano::lock_guard<nano::mutex> lock{ ctx->mutex };
	return ctx->accounts.blocked_size ();
}

std::size_t nano::bootstrap_service::score_size () const
{
	nano::lock_guard<nano::mutex> lock{ ctx->mutex };
	return ctx->scoring.size ();
}

bool nano::bootstrap_service::prioritized (nano::account const & account) const
{
	nano::lock_guard<nano::mutex> lock{ ctx->mutex };
	return ctx->accounts.prioritized (account);
}

bool nano::bootstrap_service::blocked (nano::account const & account) const
{
	nano::lock_guard<nano::mutex> lock{ ctx->mutex };
	return ctx->accounts.blocked (account);
}

auto nano::bootstrap_service::info () const -> nano::bootstrap::account_sets_index::info_t
{
	nano::lock_guard<nano::mutex> lock{ ctx->mutex };
	return ctx->accounts.info ();
}

auto nano::bootstrap_service::status () const -> status_result
{
	nano::lock_guard<nano::mutex> lock{ ctx->mutex };
	return {
		.priorities = ctx->accounts.priority_size (),
		.blocking = ctx->accounts.blocked_size (),
	};
}

nano::container_info nano::bootstrap_service::container_info () const
{
	return ctx->container_info ();
}
