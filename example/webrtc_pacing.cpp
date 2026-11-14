// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Pacing a large transfer (doc: #webrtc-channels-pacing): the sender keeps at most one MiB queued
// on its channel, and waits for the buffered amount to fall under a quarter of it before it
// sends more; the receiver reads every message as it comes. Four MiB go over in messages of
// 64 KiB.

#include <webcpp/webrtc.hpp>

#include <webcpp/asio/buffer.hpp>
#include <webcpp/asio/io_context.hpp>  // lint-world: the example's event loop

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;
using boost::system::error_code;

namespace {

constexpr std::size_t message_size = std::size_t{64} * 1024;
constexpr std::size_t messages = 64;
constexpr std::size_t high_water = std::size_t{1024} * 1024;

// tag::sender[]
// Sends `messages` messages, never more than `high_water` bytes queued at once.
class sender {
public:
    explicit sender(webrtc::data_channel& channel) : channel_(channel) {
        channel_.set_buffered_amount_low_threshold(high_water / 4);
    }

    void send() {
        while (sent_ < messages) {
            if (channel_.buffered_amount() + message_size > high_water) {
                // Too much is queued: wait until a quarter is left, then go on.
                channel_.async_wait_buffered_amount_low([this](error_code ec) {
                    if (!ec) {
                        send();
                    }
                });
                return;
            }
            if (const error_code ec = channel_.send(asio::buffer(payload_)); ec) {
                std::printf("send failed: %s\n", ec.message().c_str());
                return;
            }
            ++sent_;
        }
        std::printf("sent %zu messages, %zu bytes\n", sent_, sent_ * message_size);
    }

private:
    webrtc::data_channel& channel_;
    std::vector<char> payload_ = std::vector<char>(message_size, 'x');
    std::size_t sent_ = 0;
};

// end::sender[]

// Calls `done` with b's end once both ends of `channel` are open.
void await_open(webrtc::data_channel& channel, webrtc::peer_connection& b,
                std::function<void(webrtc::data_channel)> done) {
    channel.async_wait_open([&b, done = std::move(done)](error_code ec) mutable {
        if (ec) {
            return;
        }
        b.async_accept_data_channel(
            [done = std::move(done)](error_code ec, webrtc::data_channel remote) {
                if (!ec) {
                    done(std::move(remote));
                }
            });
    });
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
            b.async_set_remote_description(
                a.local_description().value().view(), [&](error_code ec) {
                    if (ec) {
                        return;
                    }
                    b.async_set_local_description(
                        [&](error_code ec, const webrtc::session_description&) {
                            if (ec) {
                                return;
                            }
                            gather(b, [&] {
                                a.async_set_remote_description(b.local_description().value().view(),
                                                               [](error_code) {});
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
    auto channel = a.create_data_channel("transfer").value();

    sender out(channel);
    std::optional<webrtc::data_channel> remote;
    std::vector<char> storage;
    auto buffer = asio::dynamic_buffer(storage);
    std::size_t received = 0;
    std::size_t bytes = 0;

    // tag::receiver[]
    // Reads every message as it comes, so that the channel never stalls.
    std::function<void()> receive = [&] {
        remote->async_receive(buffer, [&](error_code ec, std::size_t n, bool) {
            if (ec) {
                return;
            }
            buffer.consume(n);
            ++received;
            bytes += n;
            if (received < messages) {
                receive();
                return;
            }
            std::printf("received %zu messages, %zu bytes\n", received, bytes);
            a.close();
            b.close();
        });
    };
    // end::receiver[]

    await_open(channel, b, [&](webrtc::data_channel dc) {
        remote.emplace(std::move(dc));
        receive();
        out.send();
    });
    connect(a, b);
    io.run_for(std::chrono::seconds(30));  // lint-run: the peers' events come from libdatachannel
    return 0;
}
