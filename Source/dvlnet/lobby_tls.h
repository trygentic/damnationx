#pragma once

#include <cstdlib>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ssl/context.hpp>
#include "lobby_ca.h"

namespace devilution::net::lobby {
// OpenSSL's default CA paths may name a build machine's directories. Keep a
// Mozilla root store in the client so TLS also works on a clean installation.
inline void ConfigureTls(boost::asio::ssl::context &context)
{
    boost::system::error_code ignored;
    context.set_default_verify_paths(ignored);
    context.add_certificate_authority(boost::asio::buffer(LobbyCa, sizeof(LobbyCa) - 1));
    if (const char *ca = std::getenv("DAMNATIONX_LOBBY_CA")) context.load_verify_file(ca);
}
} // namespace devilution::net::lobby
