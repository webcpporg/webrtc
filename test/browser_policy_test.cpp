// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// What the browser cannot honour (design.md, D14; plan, Task 2.5), under node, with an
// RTCPeerConnection of the test's: `configuration::bind_address` fails `make` with
// operation_not_supported under the default policy, and is ignored under
// webcpp-webrtc-unsupported=ignore, which the test is built with too.

#include <webcpp/webrtc/peer_connection.hpp>
#include <webcpp/webrtc/runtime.hpp>

#include <webcpp/asio/error.hpp>       // lint-world: Asio's codes
#include <webcpp/asio/io_context.hpp>  // lint-world: the connection's executor

#include <webcpp/test/exit_status.hpp>

#include <emscripten/em_js.h>
#include <boost/core/lightweight_test.hpp>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;

// clang-format off
EM_JS(void, test_install_connection, (), {
    globalThis.RTCPeerConnection = class {
        constructor() {
            this.connectionState = 'new';
            this.iceConnectionState = 'new';
        }
        close() {}
    };
});
// clang-format on

int main() {
    test_install_connection();
    asio::io_context io;
    webrtc::runtime rt;
    const webrtc::configuration config{.bind_address = "127.0.0.1"};
    const auto made = webrtc::peer_connection::make(rt, io.get_executor(), config);
#ifdef WEBCPP_WEBRTC_UNSUPPORTED_IGNORE
    BOOST_TEST(made.has_value());
#else
    BOOST_TEST(made.error() == asio::error::operation_not_supported);
#endif
    const auto plain = webrtc::peer_connection::make(rt, io.get_executor(), {});
    BOOST_TEST(plain.has_value());
    return webcpp::test::exit_status(boost::report_errors());
}
