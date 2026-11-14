// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The native side of the interop test, one program that run.mjs starts once per part:
//
//   native relay            the relay (relay.hpp); prints `relay: listening on 127.0.0.1:<port>`
//   native offer <port>     the native peer that offers and sends "ping" (peer.hpp)
//   native answer <port>    the native peer that answers and replies "pong"
//
// The peer binds its candidates to 127.0.0.1 and has no ICE server, so that it gathers host
// candidates on the loopback alone. Each part exits with 0 once it has passed, else with 1, and 2
// for a usage error.

#include <webcpp/webrtc.hpp>

#include "peer.hpp"
#include "relay.hpp"

#include <webcpp/asio/io_context.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace interop = webcpp::test::interop;
namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;

namespace {

/** How long a native peer may take to reach its verdict. */
constexpr std::chrono::seconds peer_bound{60};

int usage() {
    static_cast<void>(
        std::fputs("usage: native relay | native offer <port> | native answer <port>\n", stderr));
    return 2;
}

std::optional<std::uint16_t> port_of(std::string_view text) {
    std::uint16_t port = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (ec != std::errc() || end != text.data() + text.size() || port == 0) {
        return std::nullopt;
    }
    return port;
}

int run_relay() {
    asio::io_context io;
    interop::relay relay(io);
    if (const auto ec = relay.start()) {
        std::printf("relay: failed to listen on 127.0.0.1: %s\n", ec.message().c_str());
        return 1;
    }
    std::printf("relay: listening on 127.0.0.1:%u\n", static_cast<unsigned>(relay.port()));
    static_cast<void>(std::fflush(stdout));
    io.run();  // lint-run: the relay's sockets, until both peers have left
    return relay.passed() ? 0 : 1;
}

int run_peer(interop::role role, std::uint16_t port) {
    asio::io_context io;
    webrtc::runtime rt;
    webrtc::configuration config;
    config.bind_address = "127.0.0.1";
    auto pc = webrtc::peer_connection::make(rt, io.get_executor(), config);
    if (!pc) {
        const auto name = interop::name_of(role);
        std::printf("native %.*s: failed: make: %s\n", static_cast<int>(name.size()), name.data(),
                    pc.error().message().c_str());
        return 1;
    }
    interop::peer peer(io, std::move(*pc), role, "native", peer_bound);
    peer.start(port);
    io.run();  // lint-run: the peer's events come from libdatachannel and the relay's socket
    return peer.passed() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (args.size() == 2 && std::string_view(args[1]) == "relay") {
        return run_relay();
    }
    if (args.size() != 3) {
        return usage();
    }
    const std::string_view part = args[1];
    const auto port = port_of(args[2]);
    if (!port) {
        return usage();
    }
    if (part == interop::name_of(interop::role::offer)) {
        return run_peer(interop::role::offer, *port);
    }
    if (part == interop::name_of(interop::role::answer)) {
        return run_peer(interop::role::answer, *port);
    }
    return usage();
}
