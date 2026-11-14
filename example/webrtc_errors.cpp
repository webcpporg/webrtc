// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Errors as values (doc: #webrtc-errors): what a program gets back for an ICE server URL that
// cannot be parsed, a connection past the runtime's cap, a candidate before a remote description,
// channel settings out of their range, and a send on a channel that is not open. None of them
// throws, with exceptions on or off.

#include <webcpp/webrtc.hpp>

#include <webcpp/asio/io_context.hpp>  // lint-world: the example's event loop

#include <array>
#include <cstdio>
#include <string_view>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;
using boost::system::error_code;

namespace {

void print(const char* what, const error_code& ec) {
    std::printf("%s: %s %d, %s\n", what, ec.category().name(), ec.value(), ec.message().c_str());
}

}  // namespace

int main() {
    asio::io_context io;
    // tag::errors[]
    webrtc::runtime rt(webrtc::runtime_options{.max_peer_connections = 1, .thread_pool_size = 0});

    // An ICE server URL that cannot be parsed: the W3C's SyntaxError.
    const std::array<std::string_view, 1> urls{"stun:"};
    const std::array<webrtc::ice_server, 1> servers{webrtc::ice_server{.urls = urls}};
    webrtc::configuration bad;
    bad.ice_servers = servers;
    print("a malformed STUN URL",
          webrtc::peer_connection::make(rt, io.get_executor(), bad).error());

    // The runtime's cap: one connection at a time.
    auto pc = webrtc::peer_connection::make(rt, io.get_executor(), webrtc::configuration{}).value();
    print("a second connection",
          webrtc::peer_connection::make(rt, io.get_executor(), webrtc::configuration{}).error());

    // A candidate needs a remote description first.
    pc.async_add_ice_candidate(
        webrtc::ice_candidate_view{
            .candidate = "candidate:1 1 UDP 1 "
                         "127.0.0.1 9 typ host",
        },
        [](error_code ec) { print("a candidate too early", ec); });

    // Both reliability limits at once.
    print("a channel with both limits",
          pc.create_data_channel(
                "x", webrtc::data_channel_init{.max_packet_life_time = 100, .max_retransmits = 3})
              .error());

    // A channel is not open before the connection is.
    auto channel = pc.create_data_channel("y").value();
    print("a send before the channel is open", channel.send_text("too soon"));
    // end::errors[]

    io.poll();
    return 0;
}
