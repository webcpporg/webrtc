// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Two peers in one page, signalled in memory, as ../pair.hpp's two native peers are, whose trickle
// and negotiation it reuses: the first offers, the second answers, and each trickles its
// candidates to the other until its gathering ends. A browser cannot honour bind_address, so the
// peers give none, and gather host candidates on the page's own interface, with no STUN server
// (run.mjs says which flag Chrome needs for them).

#ifndef WEBCPP_TEST_WEBRTC_BROWSER_PAGE_PAIR_HPP
#define WEBCPP_TEST_WEBRTC_BROWSER_PAGE_PAIR_HPP

#include <webcpp/webrtc.hpp>

#include "../pair.hpp"
#include "../require.hpp"

#include <webcpp/asio/io_context.hpp>  // lint-world: the tests' event loop

#include <boost/core/lightweight_test.hpp>

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace webcpp::test {

/** A configuration with host candidates only, no STUN and no bind_address, as a page allows. */
inline webrtc::configuration page_configuration() {
    return webrtc::configuration{};
}

/**
 An io_context, a runtime, and two peers of one page made with `config`, `a` with `for_a` and `b`
 with `for_b`.
*/
struct page_pair {
    /** Both peers made with `opts`. */
    explicit page_pair(const webrtc::options& opts = webrtc::options()) : page_pair(opts, opts) {}

    page_pair(const webrtc::options& for_a, const webrtc::options& for_b,
              const webrtc::configuration& config = page_configuration()) {
        auto first = webrtc::peer_connection::make(rt, io.get_executor(), config, for_a);
        auto second = webrtc::peer_connection::make(rt, io.get_executor(), config, for_b);
        require(BOOST_TEST(first.has_value()));
        require(BOOST_TEST(second.has_value()));
        a.emplace(std::move(*first));
        b.emplace(std::move(*second));
        ab.emplace(*a, *b);
        ba.emplace(*b, *a);
    }

    /** Negotiates the two peers, trickling candidates both ways. */
    void negotiate(const std::function<void(webrtc::detail::error_code)>& done) {
        test::negotiate(*a, *b, *ab, *ba, done);
    }

    /** Runs the exchange until no work is left, or the limit passes. */
    void run() {
        io.run_for(exchange_limit);  // lint-run: the peers' events come from the browser
    }

    webcpp::asio::io_context io;
    webrtc::runtime rt;
    std::optional<webrtc::peer_connection> a;
    std::optional<webrtc::peer_connection> b;
    std::optional<trickle<webrtc::peer_connection>> ab;
    std::optional<trickle<webrtc::peer_connection>> ba;
};

/**
 Opens one channel labelled `label` from `p.a` to `p.b`, and calls `opened` with both ends once
 both are open.
*/
inline void open_channel(page_pair& p, std::string_view label,
                         std::function<void(webrtc::data_channel, webrtc::data_channel)> opened,
                         const webrtc::data_channel_init& init = {}) {
    auto created = p.a->create_data_channel(label, init);
    require(BOOST_TEST(created.has_value()));

    struct state {
        std::optional<webrtc::data_channel> local;
        std::optional<webrtc::data_channel> remote;
        bool local_open = false;
        bool remote_open = false;
        std::function<void(webrtc::data_channel, webrtc::data_channel)> opened;

        void check() {
            if (local_open && remote_open) {
                opened(std::move(*local), std::move(*remote));
            }
        }
    };

    const auto s = std::make_shared<state>(state{
        .local = std::move(*created),
        .remote = std::nullopt,
        .local_open = false,
        .remote_open = false,
        .opened = std::move(opened),
    });
    s->local->async_wait_open([s](webrtc::detail::error_code ec) {
        BOOST_TEST(!ec);
        s->local_open = !ec;
        s->check();
    });
    p.b->async_accept_data_channel([s](webrtc::detail::error_code ec, webrtc::data_channel dc) {
        if (!BOOST_TEST(!ec)) {
            return;
        }
        s->remote.emplace(std::move(dc));
        s->remote->async_wait_open([s](webrtc::detail::error_code ec) {
            BOOST_TEST(!ec);
            s->remote_open = !ec;
            s->check();
        });
    });
    p.negotiate([](webrtc::detail::error_code ec) { BOOST_TEST(!ec); });
}

/**
 Ends a test program of the page: lightweight_test's summary, then `passed` when no check failed,
 which run.mjs waits for, else a line it fails on at once.
*/
inline int finish(const char* passed) {
    const int errors = boost::report_errors();
    // What stdout says is what run.mjs reads; a failure to write it fails the run there.
    if (errors == 0) {
        static_cast<void>(std::puts(passed));
    } else {
        static_cast<void>(std::printf("check failed: %d errors\n", errors));
    }
    return errors;
}

}  // namespace webcpp::test

#endif  // WEBCPP_TEST_WEBRTC_BROWSER_PAGE_PAIR_HPP
