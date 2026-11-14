// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The relay of the interop test, natively, over webcpp::beast's WebSocket: it listens on
// 127.0.0.1 alone, at a port the system chooses, takes the first two peers that connect, and
// carries every frame one sends to the other, in order, as it was sent. It reads none of them:
// what the peers say is theirs (signalling.hpp). A frame for a peer that has not finished its
// handshake yet waits for it; one for a peer that has left is dropped, and counted. It ends once
// both peers have left, and has passed when both closed their WebSocket themselves.

#ifndef WEBCPP_TEST_WEBRTC_INTEROP_RELAY_HPP
#define WEBCPP_TEST_WEBRTC_INTEROP_RELAY_HPP

#include <webcpp/asio/buffer.hpp>
#include <webcpp/asio/io_context.hpp>
#include <webcpp/asio/ip/address.hpp>
#include <webcpp/asio/ip/tcp.hpp>
#include <webcpp/beast/core.hpp>
#include <webcpp/beast/tcp_stream.hpp>
#include <webcpp/beast/websocket.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <utility>

namespace webcpp::test::interop {

/** The relay: two peers, each frame of one carried to the other. */
class relay {
    using error_code = boost::system::error_code;
    using tcp = webcpp::asio::ip::tcp;

public:
    /**
     A relay on `io`, not listening yet.

     @param io The io_context it runs on.
    */
    explicit relay(webcpp::asio::io_context& io) : acceptor_(io) {}

    /**
     Listens on 127.0.0.1 at a port the system chooses, and accepts the two peers.

     @return The failure to listen, or nothing.
    */
    error_code start() {
        const tcp::endpoint loopback(webcpp::asio::ip::make_address_v4("127.0.0.1"), 0);
        error_code ec;
        if (acceptor_.open(loopback.protocol(), ec)) {
            return ec;
        }
        if (acceptor_.bind(loopback, ec)) {
            return ec;
        }
        if (acceptor_.listen(webcpp::asio::socket_base::max_listen_connections, ec)) {
            return ec;
        }
        accept();
        return {};
    }

    /** The port it listens at, once started. */
    [[nodiscard]] std::uint16_t port() const {
        error_code ec;
        return acceptor_.local_endpoint(ec).port();
    }

    /** Whether both peers came, and left by closing their WebSocket. */
    [[nodiscard]] bool passed() const noexcept {
        return accepted_ == peers_.size() && closed_ == peers_.size() && failed_ == 0;
    }

private:
    // One peer: its WebSocket, the frame it is reading, and what waits to be written to it.
    struct peer {
        explicit peer(tcp::socket socket) : ws(std::move(socket)) {}

        webcpp::beast::websocket::stream<webcpp::beast::tcp_stream> ws;
        webcpp::beast::flat_buffer frame;
        std::deque<std::string> outbox;
        std::size_t number = 0;
        std::size_t carried = 0;
        bool joined = false;
        bool writing = false;
        bool left = false;
    };

    static void log(const std::string& what) {
        std::printf("relay: %s\n", what.c_str());
        static_cast<void>(std::fflush(stdout));
    }

    void accept() {
        acceptor_.async_accept([this](error_code ec, tcp::socket socket) {
            if (ec) {
                log("accepting a peer: " + ec.message());
                ++failed_;
                return;
            }
            auto& joining = peers_.at(accepted_);
            joining = std::make_unique<peer>(std::move(socket));
            joining->number = ++accepted_;
            // What the first peer said before the second connected, in its order.
            if (joining->number == 2) {
                joining->outbox = std::move(pending_);
                pending_.clear();
            }
            joining->ws.async_accept(
                [this, &joining = *joining](error_code ec) { on_accept(joining, ec); });
            if (accepted_ != peers_.size()) {
                accept();
            } else if (acceptor_.close(ec)) {
                log("closing the acceptor: " + ec.message());
                ++failed_;
            }
        });
    }

    void on_accept(peer& joining, error_code ec) {
        if (ec) {
            leave(joining, "its handshake failed: " + ec.message(), false);
            return;
        }
        joining.joined = true;
        joining.ws.text(true);
        log("peer " + std::to_string(joining.number) + " joined");
        write_next(joining);
        read(joining);
    }

    peer* other(const peer& one) {
        const auto& candidate = peers_.at(one.number == 1 ? 1 : 0);
        return candidate ? candidate.get() : nullptr;
    }

    // An asynchronous loop, which misc-no-recursion reads as recursion: each step starts from the
    // completion of the one before, which the io_context runs, never inside the call that started
    // it.
    // NOLINTNEXTLINE(misc-no-recursion)
    void read(peer& from) {
        // NOLINTNEXTLINE(misc-no-recursion)
        from.ws.async_read(from.frame, [this, &from](error_code ec, std::size_t) {
            if (ec == webcpp::beast::websocket::error::closed) {
                leave(from, "it closed its WebSocket", true);
                return;
            }
            if (ec) {
                leave(from, "reading from it failed: " + ec.message(), false);
                return;
            }
            carry(from);
            read(from);
        });
    }

    // The frame `from` read goes to the other peer, or waits for it to join.
    void carry(peer& from) {
        auto text = webcpp::beast::buffers_to_string(from.frame.data());
        from.frame.consume(from.frame.size());
        ++from.carried;
        peer* to = other(from);
        if (to == nullptr) {
            pending_.push_back(std::move(text));
            return;
        }
        if (to->left) {
            ++dropped_;
            return;
        }
        to->outbox.push_back(std::move(text));
        if (to->joined) {
            write_next(*to);
        }
    }

    // Another asynchronous loop, as read's.
    // NOLINTNEXTLINE(misc-no-recursion)
    void write_next(peer& to) {
        if (to.writing || to.left || to.outbox.empty()) {
            return;
        }
        to.writing = true;
        // NOLINTNEXTLINE(misc-no-recursion)
        auto written = [this, &to](error_code ec, std::size_t) {
            to.writing = false;
            to.outbox.pop_front();
            if (ec) {
                leave(to, "writing to it failed: " + ec.message(), false);
                return;
            }
            write_next(to);
        };
        to.ws.async_write(webcpp::asio::buffer(to.outbox.front()), std::move(written));
    }

    void leave(peer& gone, const std::string& why, bool closed) {
        if (gone.left) {
            return;
        }
        gone.left = true;
        if (closed) {
            ++closed_;
        } else {
            ++failed_;
        }
        dropped_ += gone.outbox.size();
        log("peer " + std::to_string(gone.number) + " left: " + why + "; it sent " +
            std::to_string(gone.carried) + " frames");
        if (closed_ + failed_ == peers_.size()) {
            log("both peers left; " + std::to_string(dropped_) + " frames were dropped");
        }
    }

    tcp::acceptor acceptor_;
    std::array<std::unique_ptr<peer>, 2> peers_;
    std::deque<std::string> pending_;
    std::size_t accepted_ = 0;
    std::size_t closed_ = 0;
    std::size_t failed_ = 0;
    std::size_t dropped_ = 0;
};

}  // namespace webcpp::test::interop

#endif  // WEBCPP_TEST_WEBRTC_INTEROP_RELAY_HPP
