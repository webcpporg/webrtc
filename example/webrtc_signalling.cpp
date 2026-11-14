// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Signalling with trickled candidates (doc: #webrtc-signalling-trickle): each peer sends its
// description at once and each candidate as soon as it is gathered, through a signalling channel,
// which here is an inbox in memory per peer. A peer applies what arrives in order, one operation
// at a time, so that a description is applied before the candidates that follow it.

#include <webcpp/webrtc.hpp>

#include <webcpp/asio/io_context.hpp>  // lint-world: the example's event loop

#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <optional>
#include <utility>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;
using boost::system::error_code;

namespace {

// tag::inbox[]
// What one peer says to the other: its description, or one of its candidates.
struct signalling_message {
    std::optional<webrtc::session_description> description;
    webrtc::ice_candidate candidate;
};

// One peer's end of the signalling channel: what the other peer said, applied in the order it was
// said, one operation at a time.
class inbox {
public:
    explicit inbox(webrtc::peer_connection& pc) : pc_(pc) {}

    // What to do once a remote description is applied.
    std::function<void()> on_description;

    // The signalling channel delivers a signal.
    void deliver(signalling_message s) {
        waiting_.push_back(std::move(s));
        if (!busy_) {
            next();
        }
    }

private:
    void next() {
        busy_ = !waiting_.empty();
        if (!busy_) {
            return;
        }
        const signalling_message s = std::move(waiting_.front());
        waiting_.pop_front();
        if (s.description) {
            pc_.async_set_remote_description(s.description->view(), [this](error_code ec) {
                if (ec) {
                    return;
                }
                on_description();
                next();
            });
        } else {
            pc_.async_add_ice_candidate(s.candidate.view(), [this](error_code ec) {
                if (!ec) {
                    next();
                }
            });
        }
    }

    webrtc::peer_connection& pc_;
    std::deque<signalling_message> waiting_;
    bool busy_ = false;
};

// end::inbox[]

// tag::trickle[]
// Sends each candidate `from` gathers to `to` as soon as it is gathered, the end of gathering, an
// empty candidate, included.
void trickle(webrtc::peer_connection& from, inbox& to) {
    from.async_wait_ice_candidate([&from, &to](error_code ec, webrtc::ice_candidate c) {
        if (ec) {
            return;
        }
        const bool end = c.candidate.empty();
        to.deliver(signalling_message{.description = std::nullopt, .candidate = std::move(c)});
        if (!end) {
            trickle(from, to);
        }
    });
}

// end::trickle[]

// Waits until `pc` is connected, then calls `done`.
void await_connected(webrtc::peer_connection& pc, std::function<void()> done) {
    pc.async_wait_state_change(
        [&pc, done = std::move(done)](error_code ec, webrtc::peer_connection_state state) mutable {
            if (ec) {
                return;
            }
            if (state == webrtc::peer_connection_state::connected) {
                done();
            } else {
                await_connected(pc, std::move(done));
            }
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
    // A description needs something to negotiate: a channel.
    const auto channel = a.create_data_channel("signalling").value();

    // tag::offer[]
    inbox at_a(a);
    inbox at_b(b);
    // b answers the offer once it is applied, then trickles its own candidates.
    at_b.on_description = [&] {
        std::printf("b: the offer is applied\n");
        b.async_set_local_description([&](error_code ec, webrtc::session_description answer) {
            if (ec) {
                return;
            }
            at_a.deliver(signalling_message{.description = std::move(answer), .candidate = {}});
            trickle(b, at_a);
        });
    };
    at_a.on_description = [] { std::printf("a: the answer is applied\n"); };

    // a offers, without waiting for its candidates, and trickles them after the offer.
    a.async_set_local_description([&](error_code ec, webrtc::session_description offer) {
        if (ec) {
            return;
        }
        at_b.deliver(signalling_message{.description = std::move(offer), .candidate = {}});
        trickle(a, at_b);
    });
    // end::offer[]

    await_connected(a, [&] {
        std::printf("a: connected\n");
        a.close();
        b.close();
    });
    io.run_for(std::chrono::seconds(30));  // lint-run: the peers' events come from libdatachannel
    return 0;
}
