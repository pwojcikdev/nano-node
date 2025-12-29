#pragma once

#include <nano/lib/numbers.hpp>

namespace nano
{
/**
 * A key pair. The private key is generated from the random pool, or passed in
 * as a hex string. The public key is derived using ed25519.
 */
class keypair
{
public:
	keypair ();
	keypair (std::string const &);
	keypair (nano::raw_key &&);
	nano::public_key pub;
	nano::raw_key prv;
};
}
