// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The page's peer of the interop test (page.html loads it, run.mjs drives it): webcpp::webrtc in
// the browser, which reaches the relay through webcpp::beast's browser WebSocket. It is the same
// peer as the native one (peer.hpp), on the browser's RTCPeerConnection:
//
//   browser_peer offer <port>     offers and sends "ping"
//   browser_peer answer <port>    answers and replies "pong"
//
// The page gives it its arguments from its query, and what it prints, through the module's
// print, ends with its verdict. Its configuration has no ICE server and no bind address, which
// the browser cannot honour (the table of differences): which of its addresses Chromium offers is
// Chromium's switches' business (run.mjs).

#include <webcpp/webrtc.hpp>

#include "peer.hpp"

#include <webcpp/asio/io_context.hpp>

#include <charconv>
#include <chrono>
#include <cstddef>
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

/** How long the page's peer may take to reach its verdict. */
constexpr std::chrono::seconds peer_bound{60};

std::optional<interop::role> role_of(std::string_view text) {
    if (text == interop::name_of(interop::role::offer)) {
        return interop::role::offer;
    }
    if (text == interop::name_of(interop::role::answer)) {
        return interop::role::answer;
    }
    return std::nullopt;
}

std::optional<std::uint16_t> port_of(std::string_view text) {
    std::uint16_t port = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (ec != std::errc() || end != text.data() + text.size() || port == 0) {
        return std::nullopt;
    }
    return port;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    const auto role = args.size() == 3 ? role_of(args[1]) : std::nullopt;
    const auto port = args.size() == 3 ? port_of(args[2]) : std::nullopt;
    if (!role || !port) {
        std::puts("browser: failed: the page gave no role and port (offer|answer <port>)");
        return 2;
    }
    asio::io_context io;
    webrtc::runtime rt;
    const webrtc::configuration config;
    auto pc = webrtc::peer_connection::make(rt, io.get_executor(), config);
    if (!pc) {
        const auto name = interop::name_of(*role);
        std::printf("browser %.*s: failed: make: %s\n", static_cast<int>(name.size()), name.data(),
                    pc.error().message().c_str());
        return 1;
    }
    interop::peer peer(io, std::move(*pc), *role, "browser", peer_bound);
    peer.start(*port);
    io.run();  // lint-run: the page's event loop, suspended into the browser's
    return peer.passed() ? 0 : 1;
}
