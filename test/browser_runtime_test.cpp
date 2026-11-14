// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The runtime in the browser (design.md, D6; plan, Task 2.2), under node, with an
// RTCPeerConnection of the test's, since node has none: `make` past
// runtime_options::max_peer_connections fails with too_many_peer_connections. The page's census
// counts a connection from its construction until the garbage collector frees it, as Chromium
// counts its own limit, so that a closed and destroyed connection still counts; once a collection
// frees it (node's gc, which the test exposes) `make` succeeds again. A constructor that throws
// counts nothing, and its exception arrives as its error.

#include <webcpp/webrtc/peer_connection.hpp>
#include <webcpp/webrtc/runtime.hpp>

#include <webcpp/asio/io_context.hpp>    // lint-world: the test's event loop
#include <webcpp/asio/steady_timer.hpp>  // lint-world: the test waits for a collection

#include <webcpp/test/exit_status.hpp>

#include <emscripten/em_js.h>
#include <boost/core/lightweight_test.hpp>

#include <chrono>
#include <cstddef>
#include <functional>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;
using boost::system::error_code;

// clang-format off
// An RTCPeerConnection that is made, with nothing to negotiate; or, after test_refuse(), one whose
// constructor throws a SyntaxError, as a browser's does for an ICE server URL it cannot parse.
EM_JS(void, test_install_connection, (), {
    globalThis.RTCPeerConnection = class {
        constructor() {
            this.connectionState = 'new';
            this.iceConnectionState = 'new';
        }
        close() { this.connectionState = 'closed'; }
    };
});

EM_JS(void, test_refuse, (), {
    globalThis.RTCPeerConnection = class {
        constructor() { throw new DOMException('refused by the test', 'SyntaxError'); }
    };
});

// A full collection, by node's gc, which the flag exposes.
EM_JS(void, test_collect, (), {
    require('v8').setFlagsFromString('--expose-gc');
    require('vm').runInNewContext('gc')();
});
// clang-format on

namespace {

using connection = webrtc::peer_connection;

// Collects, then runs `io` until the census counts fewer than `below` connections, or ten seconds
// passed: the FinalizationRegistry reports a collection in a task of its own, after it.
void collect_until(asio::io_context& io, const webrtc::runtime& rt, std::size_t below) {
    test_collect();
    asio::steady_timer poll(io);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::function<void()> next = [&] {
        if (rt.peer_connections() < below || std::chrono::steady_clock::now() > deadline) {
            return;
        }
        test_collect();
        poll.expires_after(std::chrono::milliseconds(10));
        poll.async_wait([&](error_code ec) {
            if (!ec) {
                next();
            }
        });
    };
    next();
    io.restart();
    io.run();  // lint-run: waits for the census to count a collection
}

void the_cap_and_the_census() {
    asio::io_context io;
    webrtc::runtime rt({.max_peer_connections = 2, .thread_pool_size = 0});
    BOOST_TEST_EQ(rt.get_options().max_peer_connections, 2u);
    const std::size_t before = rt.peer_connections();
    BOOST_TEST_EQ(before, 0u);
    {
        auto first = connection::make(rt, io.get_executor(), {});
        const auto second = connection::make(rt, io.get_executor(), {});
        BOOST_TEST(first.has_value());
        BOOST_TEST(second.has_value());
        BOOST_TEST_EQ(rt.peer_connections(), 2u);
        const auto third = connection::make(rt, io.get_executor(), {});
        BOOST_TEST(third.error() == webrtc::error::too_many_peer_connections);
        if (first) {
            first->close();
        }
    }
    // Closed and destroyed, they count until the garbage collector frees them.
    BOOST_TEST_EQ(rt.peer_connections(), 2u);
    const auto refused = connection::make(rt, io.get_executor(), {});
    BOOST_TEST(refused.error() == webrtc::error::too_many_peer_connections);

    collect_until(io, rt, 2);
    BOOST_TEST_LT(rt.peer_connections(), 2u);
    const auto made = connection::make(rt, io.get_executor(), {});
    BOOST_TEST(made.has_value());
}

void a_refused_constructor_counts_nothing() {
    asio::io_context io;
    webrtc::runtime rt;
    test_refuse();
    const std::size_t before = rt.peer_connections();
    const auto refused = connection::make(rt, io.get_executor(), {});
    BOOST_TEST(refused.error() == webrtc::error::syntax_error);
    BOOST_TEST_EQ(rt.peer_connections(), before);
    test_install_connection();
}

}  // namespace

int main() {
    test_install_connection();
    the_cap_and_the_census();
    a_refused_constructor_counts_nothing();
    // The io_context's run suspends main, whose returned status Asyncify loses.
    return webcpp::test::exit_status(boost::report_errors());
}
