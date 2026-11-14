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
//
// Its loops are coroutines, so that each is a loop: a completion handler that started the next
// read or write would be a recursion of the call graph.

#ifndef WEBCPP_TEST_WEBRTC_INTEROP_RELAY_HPP
#define WEBCPP_TEST_WEBRTC_INTEROP_RELAY_HPP

#include <webcpp/asio/as_tuple.hpp>
#include <webcpp/asio/awaitable.hpp>
#include <webcpp/asio/buffer.hpp>
#include <webcpp/asio/co_spawn.hpp>
#include <webcpp/asio/detached.hpp>
#include <webcpp/asio/io_context.hpp>
#include <webcpp/asio/ip/address.hpp>
#include <webcpp/asio/ip/tcp.hpp>
#include <webcpp/asio/use_awaitable.hpp>
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
        webcpp::asio::co_spawn(acceptor_.get_executor(), accept(), webcpp::asio::detached);
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
        explicit peer(tcp::socket socket, std::size_t n) : ws(std::move(socket)), number(n) {}

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

    // Accepts the two peers, then listens no more.
    webcpp::asio::awaitable<void> accept() {
        for (auto& joining : peers_) {
            auto [ec, socket] = co_await acceptor_.async_accept(
                webcpp::asio::as_tuple(webcpp::asio::use_awaitable));
            if (ec) {
                log("accepting a peer: " + ec.message());
                ++failed_;
                co_return;
            }
            joining = std::make_unique<peer>(std::move(socket), ++accepted_);
            // What the first peer said before the second connected, in its order.
            if (joining->number == 2) {
                joining->outbox = std::move(pending_);
                pending_.clear();
            }
            webcpp::asio::co_spawn(acceptor_.get_executor(), serve(*joining),
                                   webcpp::asio::detached);
        }
        if (error_code ec; acceptor_.close(ec)) {
            log("closing the acceptor: " + ec.message());
            ++failed_;
        }
    }

    // Takes a peer's WebSocket handshake, then carries each frame it sends until it leaves.
    webcpp::asio::awaitable<void> serve(peer& from) {
        const auto [accepted] =
            co_await from.ws.async_accept(webcpp::asio::as_tuple(webcpp::asio::use_awaitable));
        if (accepted) {
            leave(from, "its handshake failed: " + accepted.message(), false);
            co_return;
        }
        from.joined = true;
        from.ws.text(true);
        log("peer " + std::to_string(from.number) + " joined");
        start_writing(from);
        for (;;) {
            const auto [read, size] = co_await from.ws.async_read(
                from.frame, webcpp::asio::as_tuple(webcpp::asio::use_awaitable));
            if (read == webcpp::beast::websocket::error::closed) {
                leave(from, "it closed its WebSocket", true);
                co_return;
            }
            if (read) {
                leave(from, "reading from it failed: " + read.message(), false);
                co_return;
            }
            carry(from, size);
        }
    }

    peer* other(const peer& one) {
        const auto& candidate = peers_.at(one.number == 1 ? 1 : 0);
        return candidate ? candidate.get() : nullptr;
    }

    // The frame `from` read goes to the other peer, or waits for it to join.
    void carry(peer& from, std::size_t size) {
        auto text = webcpp::beast::buffers_to_string(from.frame.data());
        from.frame.consume(size);
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
        start_writing(*to);
    }

    void start_writing(peer& to) {
        if (to.joined && !to.writing && !to.left && !to.outbox.empty()) {
            to.writing = true;
            webcpp::asio::co_spawn(acceptor_.get_executor(), write(to), webcpp::asio::detached);
        }
    }

    // Writes what waits for `to`, in turn, until none is left.
    webcpp::asio::awaitable<void> write(peer& to) {
        while (!to.outbox.empty() && !to.left) {
            const auto [written, size] =
                co_await to.ws.async_write(webcpp::asio::buffer(to.outbox.front()),
                                           webcpp::asio::as_tuple(webcpp::asio::use_awaitable));
            to.outbox.pop_front();
            if (written) {
                to.writing = false;
                leave(to, "writing to it failed: " + written.message(), false);
                co_return;
            }
        }
        to.writing = false;
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
