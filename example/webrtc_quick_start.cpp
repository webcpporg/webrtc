// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The quick start (doc: #webrtc-quick-start): two peers in one program open a data channel and
// exchange one message each way. A program carries each description to the other peer through a
// signalling channel of its own, a WebSocket or an HTTP request; these two peers hand theirs over
// directly. Each waits until it has gathered its candidates, so that its description holds them
// all and nothing else needs to be sent; webrtc_signalling.cpp trickles them instead.

#include <webcpp/webrtc.hpp>

#include <webcpp/asio/buffer.hpp>
#include <webcpp/asio/io_context.hpp>  // lint-world: the example's event loop

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;
using boost::system::error_code;

namespace {

// tag::gather[]
// Calls `done` once `pc` has gathered its candidates, whose end is an empty candidate: its local
// description then holds every one of them.
void gather(webrtc::peer_connection& pc, std::function<void()> done) {
    pc.async_wait_ice_candidate(
        [&pc, done = std::move(done)](error_code ec, const webrtc::ice_candidate& c) mutable {
            if (ec) {
                return;
            }
            if (c.candidate.empty()) {
                done();
            } else {
                gather(pc, std::move(done));
            }
        });
}

// end::gather[]

}  // namespace

int main() {
    // tag::make[]
    asio::io_context io;
    webrtc::runtime rt;  // libdatachannel's threads, made before any connection

    // Host candidates on the loopback only, so that the example needs no network.
    webrtc::configuration config;
    config.bind_address = "127.0.0.1";

    auto a = webrtc::peer_connection::make(rt, io.get_executor(), config).value();
    auto b = webrtc::peer_connection::make(rt, io.get_executor(), config).value();
    // end::make[]

    // tag::channel[]
    // a creates the channel; b accepts it once the connection is up.
    auto chat = a.create_data_channel("chat").value();
    std::optional<webrtc::data_channel> remote;
    std::string at_a;
    std::string at_b;
    auto buffer_a = asio::dynamic_buffer(at_a);
    auto buffer_b = asio::dynamic_buffer(at_b);

    chat.async_wait_open([&](error_code ec) {
        if (ec) {
            return;
        }
        std::printf("a: the channel is open\n");
        chat.send_text("hello");
        chat.async_receive(buffer_a, [&](error_code ec, std::size_t, bool) {
            if (ec) {
                return;
            }
            std::printf("a received: %s\n", at_a.c_str());
            a.close();
            b.close();
        });
    });
    b.async_accept_data_channel([&](error_code ec, webrtc::data_channel dc) {
        if (ec) {
            return;
        }
        remote.emplace(std::move(dc));
        remote->async_receive(buffer_b, [&](error_code ec, std::size_t, bool text) {
            if (ec) {
                return;
            }
            std::printf("b received: %s (%s)\n", at_b.c_str(), text ? "text" : "binary");
            remote->send_text("hi");
        });
    });
    // end::channel[]

    // tag::signal[]
    // a offers, b answers; each description goes over once its candidates are gathered.
    a.async_set_local_description([&](error_code ec, const webrtc::session_description&) {
        if (ec) {
            return;
        }
        gather(a, [&] {
            const auto offer = a.local_description().value();
            b.async_set_remote_description(offer.view(), [&](error_code ec) {
                if (ec) {
                    return;
                }
                b.async_set_local_description(
                    [&](error_code ec, const webrtc::session_description&) {
                        if (ec) {
                            return;
                        }
                        gather(b, [&] {
                            const auto answer = b.local_description().value();
                            a.async_set_remote_description(answer.view(), [](error_code) {});
                        });
                    });
            });
        });
    });
    // end::signal[]

    // tag::run[]
    // Until both connections are closed, or the example gives up.
    io.run_for(std::chrono::seconds(30));  // lint-run: the peers' events come from libdatachannel
    // end::run[]
    return 0;
}
