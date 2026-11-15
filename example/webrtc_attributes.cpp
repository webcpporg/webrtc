// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A channel's attributes (doc: #webrtc-channels-attributes): a creates a channel whose messages
// may arrive out of order and are retransmitted three times at most, for a subprotocol of its
// own; b accepts it, and reads what a announced, which is what a reads.

#include <webcpp/webrtc.hpp>

#include <webcpp/asio/io_context.hpp>  // lint-world: the example's event loop

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;
using boost::system::error_code;

namespace {

// tag::read[]
// What one end of a channel reads of it, which never changes.
struct attributes {
    std::string label{};
    bool ordered = true;
    std::optional<std::uint16_t> max_packet_life_time{};
    std::optional<std::uint16_t> max_retransmits{};
    std::string protocol{};
    bool negotiated = false;
};

attributes attributes_of(const webrtc::data_channel& channel) {
    return {
        .label = std::string(channel.label()),
        .ordered = channel.ordered(),
        .max_packet_life_time = channel.max_packet_life_time(),
        .max_retransmits = channel.max_retransmits(),
        .protocol = std::string(channel.protocol()),
        .negotiated = channel.negotiated(),
    };
}

// end::read[]

// A limit, or "none" for a channel without it.
std::string limit(std::optional<std::uint16_t> value) {
    return value ? std::to_string(*value) : std::string("none");
}

// A flag, as JavaScript prints one.
const char* flag(bool value) {
    return value ? "true" : "false";
}

// Prints what each end reads, side by side.
void print(const attributes& created, const attributes& accepted) {
    const auto row = [](const char* name, const std::string& at_a, const std::string& at_b) {
        std::printf("%-22s%-10s%s\n", name, at_a.c_str(), at_b.c_str());
    };
    row("", "created", "accepted");
    row("label", created.label, accepted.label);
    row("ordered", flag(created.ordered), flag(accepted.ordered));
    row("max_packet_life_time", limit(created.max_packet_life_time),
        limit(accepted.max_packet_life_time));
    row("max_retransmits", limit(created.max_retransmits), limit(accepted.max_retransmits));
    row("protocol", created.protocol, accepted.protocol);
    row("negotiated", flag(created.negotiated), flag(accepted.negotiated));
}

// Gives each peer's candidates to the other once its gathering ends, as the quick start does.
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

void connect(webrtc::peer_connection& a, webrtc::peer_connection& b) {
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
}

}  // namespace

int main() {
    asio::io_context io;
    webrtc::runtime rt;
    webrtc::configuration config;
    config.bind_address = "127.0.0.1";
    auto a = webrtc::peer_connection::make(rt, io.get_executor(), config).value();
    auto b = webrtc::peer_connection::make(rt, io.get_executor(), config).value();

    // tag::create[]
    // Messages may arrive out of order, and each is retransmitted three times at most.
    webrtc::data_channel_init init;
    init.ordered = false;
    init.max_retransmits = 3;
    init.protocol = "chat.v1";
    auto chat = a.create_data_channel("chat", init).value();
    // end::create[]

    b.async_accept_data_channel([&](error_code ec, const webrtc::data_channel& accepted) {
        if (ec) {
            return;
        }
        print(attributes_of(chat), attributes_of(accepted));
        a.close();
        b.close();
    });
    connect(a, b);
    io.run_for(std::chrono::seconds(30));  // lint-run: the peers' events come from libdatachannel
    return 0;
}
