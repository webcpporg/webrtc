// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Two peers in one page, through the browser's own RTCPeerConnection: every case of
// ../native_test.cpp that a page can run, and what the browser gives that the native target does
// not (a candidate's m-line and user name fragment, the W3C's errors by their names). Where a page
// differs, the case says so: the page's census counts a connection until it is collected, not
// until it is destroyed (design.md, D6). The Jamfile builds it in both suspension variants, as the
// build asks and without exceptions; run.mjs waits for "browser webrtc: ok".

#include <webcpp/webrtc.hpp>

#include "../require.hpp"
#include "page_pair.hpp"

#include <webcpp/asio/bind_cancellation_slot.hpp>  // lint-world: a handler's cancellation
#include <webcpp/asio/buffer.hpp>                  // lint-world: messages
#include <webcpp/asio/cancellation_signal.hpp>     // lint-world: a handler's cancellation
#include <webcpp/asio/io_context.hpp>              // lint-world: the tests' event loop
#include <webcpp/asio/steady_timer.hpp>            // lint-world: lets messages arrive unread

#include <boost/core/lightweight_test.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;
using webcpp::test::page_pair;
using webrtc::detail::error_code;

namespace {

// Waits for state changes until `until`, recording each.
void watch_states(webrtc::peer_connection& pc, std::vector<webrtc::peer_connection_state>& seen,
                  webrtc::peer_connection_state until) {
    pc.async_wait_state_change([&pc, &seen, until](error_code ec, webrtc::peer_connection_state s) {
        if (ec) {
            return;
        }
        seen.push_back(s);
        if (s != until) {
            watch_states(pc, seen, until);
        }
    });
}

// Waits for ICE state changes until the agent is connected or completed, recording each.
void watch_ice_states(webrtc::peer_connection& pc,
                      std::vector<webrtc::ice_connection_state>& seen) {
    pc.async_wait_ice_connection_state_change(
        [&pc, &seen](error_code ec, webrtc::ice_connection_state s) {
            if (ec) {
                return;
            }
            seen.push_back(s);
            if (s != webrtc::ice_connection_state::connected &&
                s != webrtc::ice_connection_state::completed) {
                watch_ice_states(pc, seen);
            }
        });
}

// The whole exchange: offer, answer, candidates to their end, a channel, binary and text both
// ways, the label, both states, and both sides closing.
class full_exchange {
public:
    using received = std::vector<std::pair<std::size_t, bool>>;

    void run() {
        watch_states(*p_.a, states_a_, webrtc::peer_connection_state::connected);
        watch_ice_states(*p_.b, ice_states_b_);
        webcpp::test::open_channel(p_, "chat",
                                   [this](webrtc::data_channel a, webrtc::data_channel b) {
                                       opened(std::move(a), std::move(b));
                                   });
        p_.run();
    }

    void check() const {
        BOOST_TEST(p_.ab->ended);
        BOOST_TEST(p_.ba->ended);
        BOOST_TEST_GT(p_.ab->gathered, 0);
        BOOST_TEST_GT(p_.ba->gathered, 0);
        BOOST_TEST_EQ(p_.ab->refused, 0);
        BOOST_TEST_EQ(p_.ba->refused, 0);
        BOOST_TEST(!states_a_.empty() &&
                   states_a_.back() == webrtc::peer_connection_state::connected);
        BOOST_TEST(!ice_states_b_.empty() &&
                   (ice_states_b_.back() == webrtc::ice_connection_state::connected ||
                    ice_states_b_.back() == webrtc::ice_connection_state::completed));
        BOOST_TEST((received_b_ == received{{4, false}, {2, true}}));
        BOOST_TEST((received_a_ == received{{4, true}}));
        BOOST_TEST_EQ(at_b_.size(), 6u);
        BOOST_TEST(at_b_.substr(4) == "ab");
        BOOST_TEST(at_a_ == "pong");
        BOOST_TEST(closed_a_);
        BOOST_TEST(closed_b_);
        BOOST_TEST(cb_.value().ready_state() == webrtc::data_channel_state::closed);
        const auto local = p_.a->local_description();
        BOOST_TEST(local && local->type == webrtc::sdp_type::offer);
        BOOST_TEST(local && local->sdp.find("a=candidate") != std::string::npos);
        BOOST_TEST(p_.a->has_remote_description());
        BOOST_TEST(p_.b->has_remote_description());
    }

private:
    void opened(webrtc::data_channel a, webrtc::data_channel b) {
        auto& ca = ca_.emplace(std::move(a));
        auto& cb = cb_.emplace(std::move(b));
        BOOST_TEST(ca.label() == "chat");
        BOOST_TEST(cb.label() == "chat");
        BOOST_TEST(ca.ready_state() == webrtc::data_channel_state::open);
        BOOST_TEST(cb.ready_state() == webrtc::data_channel_state::open);
        BOOST_TEST(ca.id().has_value());
        BOOST_TEST(ca.id() == cb.id());
        const std::array<std::byte, 4> bytes{
            std::byte{1},
            std::byte{2},
            std::byte{3},
            std::byte{4},
        };
        BOOST_TEST(!ca.send(asio::buffer(bytes)));
        BOOST_TEST(!ca.send_text("ab"));
        BOOST_TEST(!cb.send_text("pong"));
        // Two receives at b, then it waits for the close a makes after its one receive.
        receive_at_b();
        ca.async_receive(buffer_a_, [this](error_code ec, std::size_t n, bool text) {
            BOOST_TEST(!ec);
            received_a_.emplace_back(n, text);
            ca_.value().async_wait_close([this](error_code ec) {
                BOOST_TEST(!ec);
                closed_a_ = true;
            });
            ca_.value().close();
        });
    }

    void receive_at_b() {
        cb_.value().async_receive(buffer_b_, [this](error_code ec, std::size_t n, bool text) {
            BOOST_TEST(!ec);
            received_b_.emplace_back(n, text);
            if (received_b_.size() < 2) {
                receive_at_b();
                return;
            }
            cb_.value().async_wait_close([this](error_code ec) {
                BOOST_TEST(!ec);
                closed_b_ = true;
            });
        });
    }

    page_pair p_;
    std::vector<webrtc::peer_connection_state> states_a_;
    std::vector<webrtc::ice_connection_state> ice_states_b_;
    std::string at_a_;
    std::string at_b_;
    received received_a_;
    received received_b_;
    decltype(asio::dynamic_buffer(at_a_)) buffer_a_ = asio::dynamic_buffer(at_a_);
    decltype(asio::dynamic_buffer(at_b_)) buffer_b_ = asio::dynamic_buffer(at_b_);
    std::optional<webrtc::data_channel> ca_;
    std::optional<webrtc::data_channel> cb_;
    bool closed_a_ = false;
    bool closed_b_ = false;
};

void a_full_exchange() {
    full_exchange exchange;
    exchange.run();
    exchange.check();
}

// A browser's candidate carries what libdatachannel's lacks (the table of differences): its
// media stream identification, its m-line and its user name fragment, the one of the
// description it belongs to.
void a_candidate_carries_its_line_and_fragment() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made =
        webrtc::peer_connection::make(rt, io.get_executor(), webcpp::test::page_configuration());
    webcpp::test::require(BOOST_TEST(made.has_value()));
    webrtc::peer_connection& pc = *made;
    // A channel gives the offer its one m-line, the application's.
    const auto channel = pc.create_data_channel("c");
    webcpp::test::require(BOOST_TEST(channel.has_value()));
    std::optional<webrtc::ice_candidate> first;
    pc.async_set_local_description([&](error_code ec, const webrtc::session_description& offer) {
        BOOST_TEST(!ec);
        BOOST_TEST(offer.type == webrtc::sdp_type::offer);
        pc.async_wait_ice_candidate([&](error_code ec, webrtc::ice_candidate c) {
            BOOST_TEST(!ec);
            first = std::move(c);
            pc.close();
        });
    });
    io.run_for(webcpp::test::exchange_limit);  // lint-run: the candidate comes from the browser
    webcpp::test::require(BOOST_TEST(first.has_value()));
    BOOST_TEST(first->candidate.starts_with("candidate:"));
    BOOST_TEST(first->sdp_mid == "0");
    BOOST_TEST(first->sdp_m_line_index == std::optional<std::uint16_t>(0));
    BOOST_TEST(!first->username_fragment.empty());
    const auto local = pc.local_description();
    webcpp::test::require(BOOST_TEST(local.has_value()));
    BOOST_TEST(local->sdp.find("a=ice-ufrag:" + first->username_fragment) != std::string::npos);
}

// Messages are kept from the channel's creation: three sent before the first receive arrive in
// order.
void received_before_the_first_receive() {
    page_pair p;
    std::optional<webrtc::data_channel> cb;
    std::optional<webrtc::data_channel> ca;
    std::string text;
    auto buffer = asio::dynamic_buffer(text);
    asio::steady_timer later(p.io);  // lint-world: lets the messages arrive unread
    std::vector<std::string> got;
    webcpp::test::open_channel(p, "early", [&](webrtc::data_channel a, webrtc::data_channel b) {
        ca.emplace(std::move(a));
        cb.emplace(std::move(b));
        BOOST_TEST(!ca->send_text("one"));
        BOOST_TEST(!ca->send_text("two"));
        BOOST_TEST(!ca->send_text("three"));
        later.expires_after(std::chrono::milliseconds(300));
        later.async_wait([&](error_code) {
            const auto next = std::make_shared<std::function<void()>>();
            *next = [&, next] {
                cb->async_receive(buffer, [&, next](error_code ec, std::size_t n, bool) {
                    if (!BOOST_TEST(!ec)) {
                        return;
                    }
                    got.emplace_back(text.substr(0, n));
                    buffer.consume(n);
                    if (got.size() < 3) {
                        (*next)();
                    } else {
                        p.a->close();
                        p.b->close();
                    }
                });
            };
            (*next)();
        });
    });
    p.run();
    BOOST_TEST((got == std::vector<std::string>{"one", "two", "three"}));
}

// A negotiated channel: both peers make it with one id, and neither accepts it.
void a_negotiated_channel() {
    page_pair p;
    webrtc::data_channel_init init;
    init.negotiated = true;
    init.id = 7;
    auto ca = p.a->create_data_channel("x", init);
    auto cb = p.b->create_data_channel("x", init);
    webcpp::test::require(BOOST_TEST(ca.has_value()));
    webcpp::test::require(BOOST_TEST(cb.has_value()));
    asio::cancellation_signal stop_accepting;
    std::optional<error_code> accepted;
    p.b->async_accept_data_channel(asio::bind_cancellation_slot(
        stop_accepting.slot(), [&](error_code ec, webrtc::data_channel) { accepted = ec; }));
    int open = 0;
    auto opened = [&](error_code ec) {
        BOOST_TEST(!ec);
        if (++open == 2) {
            BOOST_TEST(ca->id() == std::optional<std::uint16_t>(7));
            BOOST_TEST(cb->id() == std::optional<std::uint16_t>(7));
            stop_accepting.emit(asio::cancellation_type::terminal);
            p.a->close();
            p.b->close();
        }
    };
    ca->async_wait_open(opened);
    cb->async_wait_open(opened);
    p.negotiate([](error_code ec) { BOOST_TEST(!ec); });
    p.run();
    BOOST_TEST_EQ(open, 2);
    BOOST_TEST(accepted && *accepted == asio::error::operation_aborted);
}

// The cap of peer connections. The page's census counts every RTCPeerConnection from its
// construction until it is collected, the earlier cases' included, so the cap is set one above
// what it counts now; nothing between that reading and the two makes yields to the page, so no
// collection lowers the count meanwhile. A destroyed connection still counts until it is
// collected, unlike natively.
void the_cap_of_peer_connections() {
    asio::io_context io;
    std::size_t live = 0;
    {
        const webrtc::runtime probe;
        live = probe.peer_connections();
    }
    webrtc::runtime_options capped;
    capped.max_peer_connections = live + 1;
    webrtc::runtime rt(capped);
    const auto config = webcpp::test::page_configuration();
    auto first = webrtc::peer_connection::make(rt, io.get_executor(), config);
    webcpp::test::require(BOOST_TEST(first.has_value()));
    const auto second = webrtc::peer_connection::make(rt, io.get_executor(), config);
    BOOST_TEST(second.has_error() && second.error() == webrtc::error::too_many_peer_connections);
    BOOST_TEST_EQ(rt.peer_connections(), live + 1);
    {
        const auto gone = std::move(*first);
    }
    BOOST_TEST_EQ(rt.peer_connections(), live + 1);
}

// The errors that fail as values, by the names the browser gives them (design.md, D10).
void errors_as_values() {
    asio::io_context io;
    webrtc::runtime rt;
    const auto config = webcpp::test::page_configuration();
    const std::size_t live = rt.peer_connections();

    const std::array<std::string_view, 1> bad_urls{"stun:"};
    webrtc::ice_server bad_server;
    bad_server.urls = bad_urls;
    const std::array<webrtc::ice_server, 1> bad_servers{bad_server};
    webrtc::configuration bad = config;
    bad.ice_servers = bad_servers;
    const auto refused = webrtc::peer_connection::make(rt, io.get_executor(), bad);
    BOOST_TEST(refused.has_error() && refused.error() == webrtc::error::syntax_error);
    BOOST_TEST_EQ(rt.peer_connections(), live);

    auto made = webrtc::peer_connection::make(rt, io.get_executor(), config);
    webcpp::test::require(BOOST_TEST(made.has_value()));
    webrtc::peer_connection& pc = *made;
    BOOST_TEST(pc.state() == webrtc::peer_connection_state::new_);
    BOOST_TEST(pc.ice_state() == webrtc::ice_connection_state::new_);
    BOOST_TEST(!pc.local_description());
    BOOST_TEST(!pc.has_remote_description());

    webrtc::data_channel_init both;
    both.max_packet_life_time = 100;
    both.max_retransmits = 3;
    const auto two_limits = pc.create_data_channel("x", both);
    BOOST_TEST(two_limits.has_error() && two_limits.error() == webrtc::error::type_error);
    webrtc::data_channel_init no_id;
    no_id.negotiated = true;
    const auto without_id = pc.create_data_channel("x", no_id);
    BOOST_TEST(without_id.has_error() && without_id.error() == webrtc::error::type_error);
    webrtc::data_channel_init last_id;
    last_id.negotiated = true;
    last_id.id = 65535;
    const auto reserved = pc.create_data_channel("x", last_id);
    BOOST_TEST(reserved.has_error() && reserved.error() == webrtc::error::type_error);

    auto channel = pc.create_data_channel("chat");
    webcpp::test::require(BOOST_TEST(channel.has_value()));
    BOOST_TEST(channel->ready_state() == webrtc::data_channel_state::connecting);
    BOOST_TEST(channel->send_text("early") == webrtc::error::invalid_state);

    std::optional<error_code> before_description;
    pc.async_add_ice_candidate(
        webrtc::ice_candidate_view{
            .candidate = "candidate:1 1 UDP 1 127.0.0.1 9 typ host",
            .sdp_mid = std::string_view("0"),
            .sdp_m_line_index = 0,
            .username_fragment = {},
        },
        [&](error_code ec) { before_description = ec; });
    std::optional<error_code> unparsed;
    pc.async_set_remote_description(
        webrtc::session_description_view{.type = webrtc::sdp_type::offer, .sdp = "garbage"},
        [&](error_code ec) { unparsed = ec; });
    io.run();  // lint-run: the browser settles the two promises of this test
    BOOST_TEST(before_description && *before_description == webrtc::error::invalid_state);
    BOOST_TEST(unparsed && *unparsed == webrtc::error::operation_error);
    // Adding a channel does not negotiate: no description exists yet.
    BOOST_TEST(!pc.local_description());
    BOOST_TEST(!pc.has_remote_description());

    pc.close();
    const auto on_closed = pc.create_data_channel("late");
    BOOST_TEST(on_closed.has_error() && on_closed.error() == webrtc::error::invalid_state);
}

// One wait of a kind at a time: a second completes with already_started, and the first stays
// pending until the connection closes.
void one_wait_of_a_kind() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made =
        webrtc::peer_connection::make(rt, io.get_executor(), webcpp::test::page_configuration());
    webcpp::test::require(BOOST_TEST(made.has_value()));
    std::optional<error_code> first;
    std::optional<error_code> second;
    made->async_wait_state_change(
        [&](error_code ec, webrtc::peer_connection_state) { first = ec; });
    made->async_wait_state_change([&](error_code ec, webrtc::peer_connection_state) {
        second = ec;
        BOOST_TEST(!first);
        made->close();
    });
    io.run();  // lint-run: drains the posted completions of this test
    BOOST_TEST(second && *second == asio::error::already_started);
    BOOST_TEST(first && *first == asio::error::operation_aborted);
}

// Sends past the bounds fail and leave the channel open; a message larger than the caller's
// buffer stays queued.
void bounds() {
    webrtc::options opts;
    opts.max_message_size = 1024;
    opts.max_buffered_amount = 512;
    page_pair p(opts);
    std::string small_text;
    auto small = asio::dynamic_buffer(small_text, 8);
    std::string large_text;
    auto large = asio::dynamic_buffer(large_text);
    std::optional<webrtc::data_channel> ca;
    std::optional<webrtc::data_channel> cb;
    std::optional<error_code> too_small;
    std::optional<std::size_t> later;
    webcpp::test::open_channel(p, "bounds", [&](webrtc::data_channel a, webrtc::data_channel b) {
        ca.emplace(std::move(a));
        cb.emplace(std::move(b));
        const std::vector<char> big(1025, 'x');
        BOOST_TEST(ca->send(asio::buffer(big)) == webrtc::error::message_too_large);
        const std::vector<char> over(600, 'x');
        BOOST_TEST(ca->send(asio::buffer(over)) == webrtc::error::buffered_amount_exceeded);
        BOOST_TEST(ca->ready_state() == webrtc::data_channel_state::open);
        const std::vector<char> fits(16, 'y');
        BOOST_TEST(!ca->send(asio::buffer(fits)));
        cb->async_receive(small, [&](error_code ec, std::size_t, bool) {
            too_small = ec;
            cb->async_receive(large, [&](error_code ec, std::size_t n, bool) {
                BOOST_TEST(!ec);
                later = n;
                p.a->close();
                p.b->close();
            });
        });
    });
    p.run();
    BOOST_TEST(too_small && *too_small == asio::error::no_buffer_space);
    BOOST_TEST(later == std::optional<std::size_t>(16));
    BOOST_TEST(large_text == std::string(16, 'y'));
}

// close() fires no state change: the pending wait ends with operation_aborted, the state reads
// closed, the channels close with error::closed, the peer's channel sees its close, and a wait
// started afterwards completes with error::closed.
void close_fires_no_state_change() {
    page_pair p;
    std::optional<webrtc::data_channel> ca;
    std::optional<webrtc::data_channel> cb;
    std::string unused_a;
    std::string unused_b;
    auto ba = asio::dynamic_buffer(unused_a);
    auto bb = asio::dynamic_buffer(unused_b);
    std::optional<error_code> state_wait;
    std::optional<error_code> local_receive;
    std::optional<error_code> remote_receive;
    std::optional<error_code> after_close;
    bool remote_closed = false;
    webcpp::test::open_channel(p, "c", [&](webrtc::data_channel a, webrtc::data_channel b) {
        ca.emplace(std::move(a));
        cb.emplace(std::move(b));
        // The first wait completes with the current state, connected; the second is pending
        // when the connection closes.
        p.a->async_wait_state_change([&](error_code ec, webrtc::peer_connection_state s) {
            BOOST_TEST(!ec);
            BOOST_TEST(s == webrtc::peer_connection_state::connected);
            p.a->async_wait_state_change(
                [&](error_code ec, webrtc::peer_connection_state) { state_wait = ec; });
            ca->async_receive(ba, [&](error_code ec, std::size_t, bool) { local_receive = ec; });
            cb->async_receive(bb, [&](error_code ec, std::size_t, bool) {
                remote_receive = ec;
                cb->async_wait_close([&](error_code ec) {
                    BOOST_TEST(!ec);
                    remote_closed = true;
                    p.b->close();
                });
            });
            p.a->close();
            BOOST_TEST(p.a->state() == webrtc::peer_connection_state::closed);
            p.a->async_wait_state_change(
                [&](error_code ec, webrtc::peer_connection_state) { after_close = ec; });
        });
    });
    p.run();
    BOOST_TEST(state_wait && *state_wait == asio::error::operation_aborted);
    BOOST_TEST(local_receive && *local_receive == webrtc::error::closed);
    BOOST_TEST(remote_receive && *remote_receive == webrtc::error::closed);
    BOOST_TEST(after_close && *after_close == webrtc::error::closed);
    BOOST_TEST(remote_closed);
    BOOST_TEST(ca->ready_state() == webrtc::data_channel_state::closed);
}

// A channel's close() ends its receive with operation_aborted and its close wait once closed.
void a_channel_closed_by_its_owner() {
    page_pair p;
    std::optional<webrtc::data_channel> ca;
    std::optional<webrtc::data_channel> cb;
    std::string text;
    auto buffer = asio::dynamic_buffer(text);
    std::optional<error_code> receive;
    std::optional<error_code> close_wait;
    webcpp::test::open_channel(p, "c", [&](webrtc::data_channel a, webrtc::data_channel b) {
        ca.emplace(std::move(a));
        cb.emplace(std::move(b));
        ca->async_receive(buffer, [&](error_code ec, std::size_t, bool) { receive = ec; });
        ca->async_wait_close([&](error_code ec) {
            close_wait = ec;
            p.a->close();
            p.b->close();
        });
        ca->close();
        BOOST_TEST(ca->ready_state() == webrtc::data_channel_state::closing);
    });
    p.run();
    BOOST_TEST(receive && *receive == asio::error::operation_aborted);
    BOOST_TEST(close_wait && !*close_wait);
    BOOST_TEST(ca->ready_state() == webrtc::data_channel_state::closed);
}

// Destruction with a wait pending runs its handler once, with operation_aborted; a move carries
// the pending wait; a move assignment closes the connection it replaces. The census still counts
// what was destroyed, until it is collected.
void destruction_and_moves() {
    asio::io_context io;
    webrtc::runtime rt;
    const std::size_t live = rt.peer_connections();
    int aborted = 0;
    {
        auto made = webrtc::peer_connection::make(rt, io.get_executor(),
                                                  webcpp::test::page_configuration());
        webcpp::test::require(BOOST_TEST(made.has_value()));
        made->async_wait_state_change([&](error_code ec, webrtc::peer_connection_state) {
            BOOST_TEST(ec == asio::error::operation_aborted);
            ++aborted;
        });
        const webrtc::peer_connection moved(std::move(*made));
        BOOST_TEST_EQ(rt.peer_connections(), live + 1);
    }
    io.run();  // lint-run: drains the posted completions of this test
    BOOST_TEST_EQ(aborted, 1);

    auto target =
        webrtc::peer_connection::make(rt, io.get_executor(), webcpp::test::page_configuration());
    auto source =
        webrtc::peer_connection::make(rt, io.get_executor(), webcpp::test::page_configuration());
    webcpp::test::require(BOOST_TEST(target.has_value()));
    webcpp::test::require(BOOST_TEST(source.has_value()));
    std::optional<error_code> replaced;
    std::optional<error_code> carried;
    target->async_wait_state_change(
        [&](error_code ec, webrtc::peer_connection_state) { replaced = ec; });
    source->async_wait_state_change(
        [&](error_code ec, webrtc::peer_connection_state) { carried = ec; });
    *target = std::move(*source);
    BOOST_TEST(target->state() == webrtc::peer_connection_state::new_);
    target->close();
    io.restart();
    io.run();  // lint-run: drains the posted completions of this test
    BOOST_TEST(replaced && *replaced == asio::error::operation_aborted);
    BOOST_TEST(carried && *carried == asio::error::operation_aborted);
}

// A program that re-arms its waits keeps run() alive until it closes the connection, and no
// longer: run() returns well before the exchange's limit.
void run_returns_once_closed() {
    page_pair p;
    std::optional<webrtc::data_channel> ca;
    std::optional<webrtc::data_channel> cb;
    std::vector<webrtc::peer_connection_state> seen;
    std::function<void()> rearm = [&] {
        p.b->async_wait_state_change([&](error_code ec, webrtc::peer_connection_state s) {
            if (ec) {
                return;
            }
            seen.push_back(s);
            rearm();
        });
    };
    rearm();
    webcpp::test::open_channel(p, "c", [&](webrtc::data_channel a, webrtc::data_channel b) {
        ca.emplace(std::move(a));
        cb.emplace(std::move(b));
        p.b->close();
        p.a->close();
    });
    const auto start = std::chrono::steady_clock::now();  // lint-world: measures run's return
    p.run();
    const auto took =
        std::chrono::steady_clock::now() - start;  // lint-world: measures run's return
    BOOST_TEST(took < webcpp::test::exchange_limit);
    BOOST_TEST(!seen.empty());
}

}  // namespace

int main() {
    a_full_exchange();
    a_candidate_carries_its_line_and_fragment();
    received_before_the_first_receive();
    a_negotiated_channel();
    the_cap_of_peer_connections();
    errors_as_values();
    one_wait_of_a_kind();
    bounds();
    close_fires_no_state_change();
    a_channel_closed_by_its_owner();
    destruction_and_moves();
    run_returns_once_closed();
    return webcpp::test::finish("browser webrtc: ok");
}
