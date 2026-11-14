// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// configuration::bind_address, which a browser cannot honour, under the policy for what a target
// cannot honour (design.md, D14; reference/plan.md, Task 2.5). Under the default,
// webcpp-webrtc-unsupported=error, make fails with operation_not_supported and no connection is
// made; under webcpp-webrtc-unsupported=ignore, the setting is ignored, since the browser gathers
// its own host candidates, and two peers given it connect and exchange a message. The Jamfile
// builds it under each policy, in both suspension variants, as the build asks and without
// exceptions; run.mjs waits for the line of its policy.

#include <webcpp/webrtc.hpp>

#include "../require.hpp"
#include "page_pair.hpp"

#include <webcpp/asio/buffer.hpp>      // lint-world: messages
#include <webcpp/asio/io_context.hpp>  // lint-world: the tests' event loop

#include <boost/core/lightweight_test.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;

namespace {

/** A configuration that asks for the loopback, as a native peer would. */
webrtc::configuration bound() {
    webrtc::configuration config;
    config.bind_address = "127.0.0.1";
    return config;
}

#ifdef WEBCPP_WEBRTC_UNSUPPORTED_IGNORE

using webrtc::detail::error_code;

// Ignored: two peers given bind_address connect over the page's own host candidates.
void bind_address_is_ignored() {
    webcpp::test::page_pair p(webrtc::options(), webrtc::options(), bound());
    std::optional<webrtc::data_channel> ca;
    std::optional<webrtc::data_channel> cb;
    std::string text;
    auto buffer = asio::dynamic_buffer(text);
    std::optional<std::size_t> received;
    webcpp::test::open_channel(p, "ignored", [&](webrtc::data_channel a, webrtc::data_channel b) {
        ca.emplace(std::move(a));
        cb.emplace(std::move(b));
        BOOST_TEST(!ca->send_text("bound"));
        cb->async_receive(buffer, [&](error_code ec, std::size_t n, bool) {
            BOOST_TEST(!ec);
            received = n;
            p.a->close();
            p.b->close();
        });
    });
    p.run();
    BOOST_TEST(received == std::optional<std::size_t>(5));
    BOOST_TEST(text == "bound");
}

#else

// Refused: make fails with operation_not_supported, and the census counts no connection for it.
void bind_address_is_refused() {
    asio::io_context io;
    webrtc::runtime rt;
    const std::size_t live = rt.peer_connections();
    const auto refused = webrtc::peer_connection::make(rt, io.get_executor(), bound());
    BOOST_TEST(refused.has_error() && refused.error() == asio::error::operation_not_supported);
    BOOST_TEST_EQ(rt.peer_connections(), live);
    // Without it, the same make succeeds.
    const auto made =
        webrtc::peer_connection::make(rt, io.get_executor(), webcpp::test::page_configuration());
    BOOST_TEST(made.has_value());
}

#endif

}  // namespace

int main() {
#ifdef WEBCPP_WEBRTC_UNSUPPORTED_IGNORE
    bind_address_is_ignored();
    return webcpp::test::finish("browser bind_address ignored: ok");
#else
    bind_address_is_refused();
    return webcpp::test::finish("browser bind_address refused: ok");
#endif
}
