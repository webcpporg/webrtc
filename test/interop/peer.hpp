// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A peer of the interop test, the same code natively (native.cpp) and in the page
// (browser_peer.cpp): the interface is the same on both targets, and so is webcpp::beast's
// WebSocket, which reaches the relay. The offer creates the channel "interop", sends its
// description and trickles its candidates, sends "ping" once the channel is open, and closes the
// channel when "pong" arrives; the answer applies the offer, sends its own description and
// candidates, accepts the channel, replies "pong" to "ping", and waits for the offer to close
// it. Every signal travels as the W3C's JSON (signalling.hpp), and each peer applies what it
// receives in the order it was sent, one operation at a time.
//
// It prints what it does, a line each, `<who> <role>: <what>`, and ends with one verdict,
// `<who> <role>: passed` or `<who> <role>: failed: <why>`, which run.mjs reads: natively on the
// standard output, in the page through the module's print.

#ifndef WEBCPP_TEST_WEBRTC_INTEROP_PEER_HPP
#define WEBCPP_TEST_WEBRTC_INTEROP_PEER_HPP

#include <webcpp/webrtc.hpp>

#include "signalling.hpp"

#include <webcpp/asio/buffer.hpp>
#include <webcpp/asio/io_context.hpp>
#include <webcpp/asio/ip/address.hpp>
#include <webcpp/asio/ip/tcp.hpp>
#include <webcpp/asio/steady_timer.hpp>
#include <webcpp/beast/core.hpp>
#include <webcpp/beast/tcp_stream.hpp>
#include <webcpp/beast/websocket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace webcpp::test::interop {

namespace asio = webcpp::asio;
namespace beast = webcpp::beast;
using boost::system::error_code;

/** What a peer does: offer and send "ping", or answer and reply "pong". */
enum class role {
    /** Creates the channel, offers, sends "ping" and closes the channel on "pong". */
    offer,
    /** Answers, accepts the channel, replies "pong" to "ping" and waits for the close. */
    answer,
};

/** The name of a role, as a peer prints it and as its program reads it. */
inline std::string_view name_of(role r) {
    return r == role::offer ? "offer" : "answer";
}

/** The W3C's name of a connection's state. */
inline std::string_view name_of(webrtc::peer_connection_state state) {
    using enum webrtc::peer_connection_state;
    switch (state) {
        case new_: return "new";
        case connecting: return "connecting";
        case connected: return "connected";
        case disconnected: return "disconnected";
        case failed: return "failed";
        case closed: return "closed";
    }
    return "unknown";
}

/** The W3C's name of an ICE agent's state. */
inline std::string_view name_of(webrtc::ice_connection_state state) {
    using enum webrtc::ice_connection_state;
    switch (state) {
        case new_: return "new";
        case checking: return "checking";
        case connected: return "connected";
        case completed: return "completed";
        case disconnected: return "disconnected";
        case failed: return "failed";
        case closed: return "closed";
    }
    return "unknown";
}

/** The label of the channel the offer creates. */
inline constexpr std::string_view channel_label = "interop";

/** What the offer sends once the channel is open. */
inline constexpr std::string_view ping = "ping";

/** What the answer replies to it. */
inline constexpr std::string_view pong = "pong";

/**
 One peer of the test: its connection, its WebSocket to the relay, and the exchange.

 It lives until its io_context's run returns, which happens once it has ended: it closes its
 channel, its connection and its WebSocket, and cancels its bound, whatever the verdict.
*/
class peer {
public:
    /**
     A peer that will play `r` with `pc`, and fail if it has not ended within `bound`.

     @param io The io_context it runs on, `pc`'s.
     @param pc Its connection, made on `io`'s executor.
     @param r Its role.
     @param who What its lines start with, before the role: `native` or `browser`.
     @param bound How long the whole exchange may take.
    */
    peer(asio::io_context& io, webrtc::peer_connection pc, role r, std::string_view who,
         std::chrono::seconds bound)
        : ws_(io),
          timer_(io),
          pc_(std::move(pc)),
          role_(r),
          prefix_(std::string(who) + " " + std::string(name_of(r)) + ": "),
          bound_(bound) {}

    /**
     Starts: connects to the relay on 127.0.0.1 at `port`, then plays its role.

     @param port The relay's port.
    */
    void start(std::uint16_t port) {
        timer_.expires_after(bound_);
        timer_.async_wait([this](error_code ec) {
            if (!ec) {
                fail("no verdict within " + std::to_string(bound_.count()) +
                     " s; the last step was: " + last_);
            }
        });
        watch_state();
        watch_ice_state();
        if (role_ == role::offer) {
            auto channel = pc_.create_data_channel(channel_label);
            if (!channel) {
                fail("create_data_channel", channel.error());
                return;
            }
            channel_.emplace(std::move(*channel));
            wait_open();
        } else {
            accept_channel();
        }
        const asio::ip::tcp::endpoint relay(asio::ip::make_address_v4("127.0.0.1"), port);
        beast::get_lowest_layer(ws_).async_connect(relay, [this, port](error_code ec) {
            if (done_) {
                return;
            }
            if (ec) {
                fail("connecting to the relay", ec);
                return;
            }
            ws_.async_handshake("127.0.0.1:" + std::to_string(port), "/",
                                [this](error_code ec) { on_handshake(ec); });
        });
    }

    /** Whether it ended with "passed". */
    [[nodiscard]] bool passed() const noexcept { return passed_; }

private:
    // ---- what it prints ---------------------------------------------------------------------

    void log(std::string_view what) {
        last_ = what;
        std::printf("%s%.*s\n", prefix_.c_str(), static_cast<int>(what.size()), what.data());
        static_cast<void>(std::fflush(stdout));
    }

    void fail(std::string_view why) {
        if (done_) {
            return;
        }
        log("failed: " + std::string(why));
        finish(false);
    }

    void fail(std::string_view step, error_code ec) {
        fail(std::string(step) + ": " + ec.message() + " (" + ec.category().name() + " " +
             std::to_string(ec.value()) + ")");
    }

    // ---- the end, whatever the verdict ----------------------------------------------------------

    void finish(bool passed) {
        if (done_) {
            return;
        }
        done_ = true;
        passed_ = passed;
        if (passed) {
            log("passed");
        }
        timer_.cancel();
        if (channel_) {
            channel_->close();
        }
        pc_.close();
        close_relay();
    }

    // The WebSocket closes once no write is in flight, a write's completion closing it otherwise.
    void close_relay() {
        if (writing_) {
            return;
        }
        if (!ws_.is_open()) {
            beast::get_lowest_layer(ws_).close();
            return;
        }
        ws_.async_close(beast::websocket::close_code::normal, [](error_code) {});
    }

    // ---- the relay --------------------------------------------------------------------------

    void on_handshake(error_code ec) {
        if (done_) {
            return;
        }
        if (ec) {
            fail("the WebSocket handshake with the relay", ec);
            return;
        }
        ws_.text(true);
        log("connected to the relay");
        read_signal();
        if (role_ == role::offer) {
            make_offer();
        }
    }

    void send_signal(std::string text) {
        outbox_.push_back(std::move(text));
        if (!writing_) {
            write_next();
        }
    }

    // An asynchronous loop, which misc-no-recursion reads as recursion: each step starts from the
    // completion of the one before, which the io_context runs, never inside the call that started
    // it.
    // NOLINTNEXTLINE(misc-no-recursion)
    void write_next() {
        writing_ = !outbox_.empty();
        if (!writing_) {
            return;
        }
        // NOLINTNEXTLINE(misc-no-recursion)
        ws_.async_write(asio::buffer(outbox_.front()), [this](error_code ec, std::size_t) {
            outbox_.pop_front();
            writing_ = false;
            if (done_) {
                close_relay();
                return;
            }
            if (ec) {
                fail("writing to the relay", ec);
                return;
            }
            write_next();
        });
    }

    // Another asynchronous loop, as write_next's.
    // NOLINTNEXTLINE(misc-no-recursion)
    void read_signal() {
        // NOLINTNEXTLINE(misc-no-recursion)
        ws_.async_read(frame_, [this](error_code ec, std::size_t) {
            if (done_) {
                return;
            }
            if (ec) {
                fail("reading from the relay", ec);
                return;
            }
            const auto text = beast::buffers_to_string(frame_.data());
            frame_.consume(frame_.size());
            auto signal = parse_signal(text);
            if (!signal) {
                fail("the relay carried a signal that is no W3C dictionary: " + text);
                return;
            }
            inbox_.push_back(std::move(*signal));
            apply_next();
            read_signal();
        });
    }

    // ---- what the other peer said, applied in order -------------------------------------------

    void apply_next() {
        if (applying_ || inbox_.empty()) {
            return;
        }
        applying_ = true;
        const signal_message signal = std::move(inbox_.front());
        inbox_.pop_front();
        if (signal.is_description()) {
            const auto type = signal.description().type;
            pc_.async_set_remote_description(signal.description(), [this, type](error_code ec) {
                applying_ = false;
                if (done_) {
                    return;
                }
                if (ec) {
                    fail("set_remote_description of the " + std::string(name_of(type)), ec);
                    return;
                }
                log("applied the " + std::string(name_of(type)));
                if (role_ == role::answer && type == webrtc::sdp_type::offer) {
                    make_answer();
                }
                apply_next();
            });
            return;
        }
        adding_ = signal.candidate;
        pc_.async_add_ice_candidate(signal.ice_candidate(), [this](error_code ec) {
            applying_ = false;
            if (done_) {
                return;
            }
            if (ec) {
                fail("add_ice_candidate of '" + adding_ + "'", ec);
                return;
            }
            if (adding_.empty()) {
                log("added the end of the remote candidates, after " +
                    std::to_string(added_candidates_));
            } else {
                ++added_candidates_;
            }
            apply_next();
        });
    }

    // ---- descriptions and candidates --------------------------------------------------------

    void make_offer() {
        pc_.async_set_local_description(
            [this](error_code ec, const webrtc::session_description& offer) {
                if (done_) {
                    return;
                }
                if (ec) {
                    fail("set_local_description of the offer", ec);
                    return;
                }
                send_signal(to_json(offer.view()));
                log("sent the offer");
                trickle();
            });
    }

    void make_answer() {
        pc_.async_set_local_description(
            [this](error_code ec, const webrtc::session_description& answer) {
                if (done_) {
                    return;
                }
                if (ec) {
                    fail("set_local_description of the answer", ec);
                    return;
                }
                send_signal(to_json(answer.view()));
                log("sent the answer");
                trickle();
            });
    }

    // Each local candidate goes to the relay as soon as it is gathered, the end included.
    void trickle() {
        pc_.async_wait_ice_candidate([this](error_code ec, const webrtc::ice_candidate& candidate) {
            if (done_) {
                return;
            }
            if (ec) {
                fail("wait_ice_candidate", ec);
                return;
            }
            send_signal(to_json(candidate.view()));
            if (candidate.candidate.empty()) {
                log("sent the end of its candidates");
                return;
            }
            log("sent the candidate " + std::string(candidate.candidate));
            trickle();
        });
    }

    // ---- the connection's states, for the record -----------------------------------------

    void watch_state() {
        pc_.async_wait_state_change([this](error_code ec, webrtc::peer_connection_state state) {
            if (done_ || ec) {
                return;
            }
            log("connection state " + std::string(name_of(state)));
            if (state == webrtc::peer_connection_state::failed) {
                fail("the connection failed");
                return;
            }
            watch_state();
        });
    }

    void watch_ice_state() {
        pc_.async_wait_ice_connection_state_change(
            [this](error_code ec, webrtc::ice_connection_state state) {
                if (done_ || ec) {
                    return;
                }
                log("ICE state " + std::string(name_of(state)));
                watch_ice_state();
            });
    }

    // ---- the channel ------------------------------------------------------------------------

    void accept_channel() {
        pc_.async_accept_data_channel([this](error_code ec, webrtc::data_channel channel) {
            if (done_) {
                return;
            }
            if (ec) {
                fail("accept_data_channel", ec);
                return;
            }
            log("accepted the channel '" + std::string(channel.label()) + "'");
            if (channel.label() != channel_label) {
                fail("the channel is not '" + std::string(channel_label) + "'");
                return;
            }
            channel_.emplace(std::move(channel));
            wait_open();
        });
    }

    void wait_open() {
        channel_->async_wait_open([this](error_code ec) {
            if (done_) {
                return;
            }
            if (ec) {
                fail("wait_open", ec);
                return;
            }
            log("the channel is open");
            if (role_ == role::offer) {
                if (const auto refused = channel_->send_text(ping)) {
                    fail("send_text of ping", refused);
                    return;
                }
                log("sent ping");
            }
            receive();
        });
    }

    void receive() {
        channel_->async_receive(message_, [this](error_code ec, std::size_t size, bool text) {
            if (done_) {
                return;
            }
            if (ec) {
                fail("async_receive", ec);
                return;
            }
            const auto got = beast::buffers_to_string(message_.data());
            message_.consume(size);
            const auto due = role_ == role::offer ? pong : ping;
            if (!text || got != due) {
                fail("received " + std::string(text ? "the text '" : "a binary '") + got +
                     "' where the text '" + std::string(due) + "' was due");
                return;
            }
            log("received " + got);
            if (role_ == role::answer) {
                if (const auto refused = channel_->send_text(pong)) {
                    fail("send_text of pong", refused);
                    return;
                }
                log("sent pong");
            }
            wait_close();
            // The offer ends the exchange: it closes the channel, which the answer sees closed.
            if (role_ == role::offer) {
                channel_->close();
            }
        });
    }

    void wait_close() {
        channel_->async_wait_close([this](error_code ec) {
            if (done_) {
                return;
            }
            if (ec) {
                fail("wait_close", ec);
                return;
            }
            log(role_ == role::offer ? "closed the channel" : "the offer closed the channel");
            finish(true);
        });
    }

    beast::websocket::stream<beast::tcp_stream> ws_;
    asio::steady_timer timer_;
    webrtc::peer_connection pc_;
    std::optional<webrtc::data_channel> channel_;
    role role_;
    std::string prefix_;
    std::chrono::seconds bound_;
    std::string last_ = "none";
    std::string adding_;
    beast::flat_buffer frame_;
    beast::flat_buffer message_;
    std::deque<std::string> outbox_;
    std::deque<signal_message> inbox_;
    std::size_t added_candidates_ = 0;
    bool writing_ = false;
    bool applying_ = false;
    bool done_ = false;
    bool passed_ = false;
};

}  // namespace webcpp::test::interop

#endif  // WEBCPP_TEST_WEBRTC_INTEROP_PEER_HPP
