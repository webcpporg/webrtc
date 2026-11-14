// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The browser's data channel (reference/plan.md, Task 2.4; design.md, D5, D9 and D13), two peers
// in one page: unread messages past options::max_received_bytes close the channel with
// receive_overflow, since a browser has no flow control on receipt; a received message past
// options::max_message_size closes it with message_too_large; a send refused before the browser
// sees it leaves the channel open; the buffered-amount wait; the ways a channel closes; a channel
// accepted after its messages arrived, which loses none; and the backlog of incoming channels.
// The Jamfile builds it in both suspension variants, as the build asks and without exceptions;
// run.mjs waits for "browser webrtc channels: ok".

#include <webcpp/webrtc.hpp>

#include "../require.hpp"
#include "page_pair.hpp"

#include <webcpp/asio/buffer.hpp>        // lint-world: messages
#include <webcpp/asio/io_context.hpp>    // lint-world: the tests' event loop
#include <webcpp/asio/steady_timer.hpp>  // lint-world: lets messages arrive unread

#include <boost/core/lightweight_test.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;
using webcpp::test::page_pair;
using webrtc::detail::error_code;

namespace {

/** How long a test leaves messages unread, so that the browser delivers them all first. */
constexpr std::chrono::milliseconds unread{500};

/** What read_messages reads until a receive fails. */
constexpr std::size_t until_it_fails = std::numeric_limits<std::size_t>::max();

/** The messages read from a channel, the buffer they are read into, and how the reading ended. */
struct reader {
    reader() = default;
    reader(const reader&) = delete;
    reader& operator=(const reader&) = delete;

    std::string storage;
    decltype(asio::dynamic_buffer(storage)) buffer = asio::dynamic_buffer(storage);
    std::vector<std::string> messages;
    std::vector<bool> texts;
    std::optional<error_code> end;
};

/**
 Reads `channel` into `r` until it holds `limit` messages or a receive fails, which `r.end`
 records, then calls `done`.
*/
void read_messages(webrtc::data_channel& channel, reader& r, std::size_t limit,
                   std::function<void()> done) {
    if (r.messages.size() == limit) {
        done();
        return;
    }
    channel.async_receive(r.buffer, [&channel, &r, limit, done = std::move(done)](
                                        error_code ec, std::size_t n, bool text) mutable {
        if (ec) {
            r.end = ec;
            done();
            return;
        }
        r.messages.emplace_back(r.storage.substr(0, n));
        r.texts.push_back(text);
        r.buffer.consume(n);
        read_messages(channel, r, limit, std::move(done));
    });
}

/** Checks that `got` read exactly `expected`, naming each message that differs. */
void read_exactly(const reader& got, const std::vector<std::string>& expected) {
    BOOST_TEST_ALL_EQ(got.messages.begin(), got.messages.end(), expected.begin(), expected.end());
}

/** What calls `last` on its `count`th call, and nothing before. */
std::function<void()> after(int count, std::function<void()> last) {
    const auto left = std::make_shared<int>(count);
    return [left, last = std::move(last)] {
        if (--*left == 0) {
            last();
        }
    };
}

/** A message of `size` bytes, each the letter `fill`. */
std::string message(std::size_t size, char fill) {
    std::string made;
    made.assign(size, fill);
    return made;
}

// Unread messages past max_received_bytes, charged as the browser delivers them, close the
// channel: what was read before the budget passed is whole and in order, the reading ends with
// receive_overflow, the channel is closed, and the peer sees its close.
void unread_messages_past_the_budget() {
    constexpr std::size_t sent = 16;
    constexpr std::size_t size = 1024;
    webrtc::options budget;
    budget.max_received_bytes = 8 * size;
    page_pair p(webrtc::options(), budget);
    std::optional<webrtc::data_channel> ca;
    std::optional<webrtc::data_channel> cb;
    asio::steady_timer later(p.io);  // lint-world: lets the messages arrive unread
    reader got;
    std::optional<error_code> peer_saw;
    const auto both = after(2, [&] {
        p.a->close();
        p.b->close();
    });
    webcpp::test::open_channel(p, "flood", [&](webrtc::data_channel a, webrtc::data_channel b) {
        ca.emplace(std::move(a));
        cb.emplace(std::move(b));
        for (std::size_t i = 0; i < sent; ++i) {
            const auto m = message(size, static_cast<char>('a' + i));
            BOOST_TEST(!ca->send(asio::buffer(m)));
        }
        ca->async_wait_close([&](error_code ec) {
            peer_saw = ec;
            both();
        });
        later.expires_after(unread);
        later.async_wait([&](error_code) { read_messages(*cb, got, until_it_fails, both); });
    });
    p.run();
    BOOST_TEST(got.end && *got.end == webrtc::error::receive_overflow);
    BOOST_TEST_LT(got.messages.size(), sent);
    BOOST_TEST_LE(got.messages.size() * size, budget.max_received_bytes);
    for (std::size_t i = 0; i < got.messages.size(); ++i) {
        BOOST_TEST(got.messages[i] == message(size, static_cast<char>('a' + i)));
    }
    BOOST_TEST(cb->ready_state() == webrtc::data_channel_state::closed);
    BOOST_TEST(peer_saw && !*peer_saw);
}

// A received message past max_message_size, which a browser cannot announce to its peer, closes
// the channel with message_too_large, after the messages that came before it.
void a_received_message_past_the_size() {
    webrtc::options strict;
    strict.max_message_size = 1024;
    page_pair p(webrtc::options(), strict);
    std::optional<webrtc::data_channel> ca;
    std::optional<webrtc::data_channel> cb;
    reader got;
    std::optional<error_code> peer_saw;
    const auto both = after(2, [&] {
        p.a->close();
        p.b->close();
    });
    webcpp::test::open_channel(p, "large", [&](webrtc::data_channel a, webrtc::data_channel b) {
        ca.emplace(std::move(a));
        cb.emplace(std::move(b));
        BOOST_TEST(!ca->send_text("first"));
        const auto large = message(2000, 'x');
        BOOST_TEST(!ca->send(asio::buffer(large)));
        ca->async_wait_close([&](error_code ec) {
            peer_saw = ec;
            both();
        });
        read_messages(*cb, got, until_it_fails, both);
    });
    p.run();
    read_exactly(got, {"first"});
    BOOST_TEST(got.end && *got.end == webrtc::error::message_too_large);
    BOOST_TEST(cb->ready_state() == webrtc::data_channel_state::closed);
    BOOST_TEST(peer_saw && !*peer_saw);
}

/** The limit a browser announces to its peer in its description's a=max-message-size. */
constexpr std::size_t announced = std::size_t{256} * 1024;

/** How the refused asynchronous sends of refused_sends_leave_it_open completed, and when. */
struct refusals {
    std::optional<error_code> async_send;
    std::optional<error_code> async_send_text;
    bool returned = false;
};

// Sends from `ca`, whose bounds refused_sends_leave_it_open sets, what is refused, and the
// largest message the peer allows; once the asynchronous refusals are posted, the text "after".
void refuse(webrtc::data_channel& ca, refusals& r) {
    const auto past_peer = message(announced + 1, 'p');
    BOOST_TEST(ca.send(asio::buffer(past_peer)) == webrtc::error::message_too_large);
    BOOST_TEST(ca.send_text(past_peer) == webrtc::error::message_too_large);
    const auto largest = message(announced, 'l');
    BOOST_TEST(!ca.send(asio::buffer(largest)));
    BOOST_TEST_GE(ca.buffered_amount(), announced);
    const auto more = message(std::size_t{64} * 1024, 'm');
    BOOST_TEST(ca.send(asio::buffer(more)) == webrtc::error::buffered_amount_exceeded);
    ca.async_send(asio::buffer(past_peer), [&r](error_code ec) {
        BOOST_TEST(r.returned);
        r.async_send = ec;
    });
    ca.async_send_text(past_peer, [&ca, &r](error_code ec) {
        BOOST_TEST(r.returned);
        r.async_send_text = ec;
        BOOST_TEST(ca.ready_state() == webrtc::data_channel_state::open);
        BOOST_TEST(!ca.send_text("after"));
    });
    r.returned = true;
    BOOST_TEST(ca.ready_state() == webrtc::data_channel_state::open);
}

// A send is refused before the browser sees it, and the channel stays open: past the smaller of
// max_message_size and the 256 KiB a browser announces, past max_buffered_amount, and through
// async_send and async_send_text, whose refusal is posted. The peer receives only what was
// accepted, the largest message it allows among it.
void refused_sends_leave_it_open() {
    webrtc::options sender;
    sender.max_message_size = std::size_t{1024} * 1024;
    sender.max_buffered_amount = std::size_t{300} * 1024;
    page_pair p(sender, webrtc::options());
    std::optional<webrtc::data_channel> ca;
    std::optional<webrtc::data_channel> cb;
    reader got;
    refusals r;
    webcpp::test::open_channel(p, "refusals", [&](webrtc::data_channel a, webrtc::data_channel b) {
        ca.emplace(std::move(a));
        cb.emplace(std::move(b));
        refuse(*ca, r);
        read_messages(*cb, got, 2, [&] {
            p.a->close();
            p.b->close();
        });
    });
    p.run();
    BOOST_TEST(r.async_send && *r.async_send == webrtc::error::message_too_large);
    BOOST_TEST(r.async_send_text && *r.async_send_text == webrtc::error::message_too_large);
    BOOST_TEST(!got.end);
    webcpp::test::require(BOOST_TEST_EQ(got.messages.size(), 2u));
    BOOST_TEST(got.messages[0] == message(announced, 'l'));
    BOOST_TEST(!got.texts[0]);
    BOOST_TEST(got.messages[1] == "after");
    BOOST_TEST(got.texts[1]);
}

// The buffered-amount wait completes at once, posted, at or below the threshold, and otherwise
// once the browser has handed enough to its transport.
void the_buffered_amount_wait() {
    constexpr std::size_t threshold = std::size_t{64} * 1024;
    constexpr std::size_t count = 16;
    page_pair p;
    std::optional<webrtc::data_channel> ca;
    std::optional<webrtc::data_channel> cb;
    reader got;
    std::optional<error_code> at_once;
    std::optional<error_code> low;
    std::size_t queued = 0;
    std::size_t when_low = 0;
    bool returned = false;
    const auto both = after(2, [&] {
        p.a->close();
        p.b->close();
    });
    webcpp::test::open_channel(p, "paced", [&](webrtc::data_channel a, webrtc::data_channel b) {
        ca.emplace(std::move(a));
        cb.emplace(std::move(b));
        ca->set_buffered_amount_low_threshold(threshold);
        ca->async_wait_buffered_amount_low([&](error_code ec) {
            BOOST_TEST(returned);
            at_once = ec;
            const auto chunk = message(threshold, 'c');
            for (std::size_t i = 0; i < count; ++i) {
                BOOST_TEST(!ca->send(asio::buffer(chunk)));
            }
            queued = ca->buffered_amount();
            ca->async_wait_buffered_amount_low([&](error_code ec) {
                low = ec;
                when_low = ca->buffered_amount();
                both();
            });
        });
        returned = true;
        read_messages(*cb, got, count, both);
    });
    p.run();
    BOOST_TEST(at_once && !*at_once);
    BOOST_TEST_GT(queued, threshold);
    BOOST_TEST(low && !*low);
    BOOST_TEST_LE(when_low, threshold);
    BOOST_TEST(!got.end);
    BOOST_TEST_EQ(got.messages.size(), count);
}

// The peer closes its channel: what reached the page before, unread, is still delivered after the
// close, then the reading ends with error::closed; both close waits complete without an error, and
// a send on a closed channel is refused with invalid_state. The peer closes once the messages
// reached the page: a browser discards a message that reaches a channel no longer open (the W3C's
// "receiving messages"), and Chrome 156 does so with messages sent just before a close made at
// once, measured with plain RTCDataChannel objects; 50 ms later they arrive first.
void the_peer_closes_its_channel() {
    page_pair p;
    std::optional<webrtc::data_channel> ca;
    std::optional<webrtc::data_channel> cb;
    asio::steady_timer later(p.io);  // lint-world: lets the messages reach the peer's page
    reader got;
    std::optional<error_code> closed_here;
    std::optional<error_code> closed_there;
    const auto all = after(2, [&] {
        BOOST_TEST(ca->send_text("late") == webrtc::error::invalid_state);
        BOOST_TEST(cb->send_text("late") == webrtc::error::invalid_state);
        p.a->close();
        p.b->close();
    });
    webcpp::test::open_channel(p, "c", [&](webrtc::data_channel a, webrtc::data_channel b) {
        ca.emplace(std::move(a));
        cb.emplace(std::move(b));
        BOOST_TEST(!ca->send_text("one"));
        BOOST_TEST(!ca->send_text("two"));
        later.expires_after(unread);
        later.async_wait([&](error_code) {
            ca->async_wait_close([&](error_code ec) {
                closed_here = ec;
                all();
            });
            ca->close();
        });
        // b reads nothing until the close reached it.
        cb->async_wait_close([&](error_code ec) {
            closed_there = ec;
            read_messages(*cb, got, until_it_fails, all);
        });
    });
    p.run();
    read_exactly(got, {"one", "two"});
    BOOST_TEST(got.end && *got.end == webrtc::error::closed);
    BOOST_TEST(closed_here && !*closed_here);
    BOOST_TEST(closed_there && !*closed_there);
    BOOST_TEST(cb->ready_state() == webrtc::data_channel_state::closed);
}

// Destroying a channel completes its pending operations with operation_aborted, once each, and
// closes it: the peer's close wait completes without an error.
void a_channel_destroyed() {
    page_pair p;
    std::optional<webrtc::data_channel> ca;
    std::optional<webrtc::data_channel> cb;
    reader got;
    std::optional<error_code> close_wait;
    std::optional<error_code> peer_saw;
    int completions = 0;
    const auto all = after(3, [&] {
        p.a->close();
        p.b->close();
    });
    webcpp::test::open_channel(p, "c", [&](webrtc::data_channel a, webrtc::data_channel b) {
        ca.emplace(std::move(a));
        cb.emplace(std::move(b));
        ca->async_wait_close([&](error_code ec) {
            peer_saw = ec;
            all();
        });
        read_messages(*cb, got, until_it_fails, [&] {
            ++completions;
            all();
        });
        cb->async_wait_close([&](error_code ec) {
            ++completions;
            close_wait = ec;
            all();
        });
        cb.reset();
    });
    p.run();
    BOOST_TEST(got.messages.empty());
    BOOST_TEST(got.end && *got.end == asio::error::operation_aborted);
    BOOST_TEST(close_wait && *close_wait == asio::error::operation_aborted);
    BOOST_TEST_EQ(completions, 2);
    BOOST_TEST(peer_saw && !*peer_saw);
}

// A connection closed before its channel opened closes the channel: the open wait completes with
// error::closed, the close wait without an error, and an open wait started then with
// error::closed too.
void closed_before_it_opened() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made =
        webrtc::peer_connection::make(rt, io.get_executor(), webcpp::test::page_configuration());
    webcpp::test::require(BOOST_TEST(made.has_value()));
    auto channel = made->create_data_channel("never");
    webcpp::test::require(BOOST_TEST(channel.has_value()));
    std::optional<error_code> open;
    std::optional<error_code> close_wait;
    std::optional<error_code> open_later;
    channel->async_wait_open([&](error_code ec) {
        open = ec;
        channel->async_wait_open([&](error_code ec) { open_later = ec; });
    });
    channel->async_wait_close([&](error_code ec) { close_wait = ec; });
    made->close();
    io.run();  // lint-run: drains the posted completions of this test
    BOOST_TEST(open && *open == webrtc::error::closed);
    BOOST_TEST(close_wait && !*close_wait);
    BOOST_TEST(open_later && *open_later == webrtc::error::closed);
    BOOST_TEST(channel->ready_state() == webrtc::data_channel_state::closed);
    BOOST_TEST(channel->send_text("x") == webrtc::error::invalid_state);
}

// Accepts the next channel of `p.b` into `cb`, open and labelled "late", and reads `count`
// messages from it into `got`; then both peers close.
void accept_and_read(page_pair& p, std::optional<webrtc::data_channel>& cb, reader& got,
                     std::size_t count) {
    p.b->async_accept_data_channel([&p, &cb, &got, count](error_code ec, webrtc::data_channel dc) {
        if (!BOOST_TEST(!ec)) {
            return;
        }
        cb.emplace(std::move(dc));
        BOOST_TEST(cb->label() == "late");
        BOOST_TEST(cb->ready_state() == webrtc::data_channel_state::open);
        read_messages(*cb, got, count, [&p] {
            p.a->close();
            p.b->close();
        });
    });
}

// A channel accepted after its messages arrived loses none: the browser opens it and delivers
// them before the program accepts it, and they wait with it in the queue of incoming channels.
void accepted_after_its_messages() {
    page_pair p;
    auto created = p.a->create_data_channel("late");
    webcpp::test::require(BOOST_TEST(created.has_value()));
    webrtc::data_channel& ca = *created;
    std::optional<webrtc::data_channel> cb;
    asio::steady_timer later(p.io);  // lint-world: accepts once the messages arrived
    reader got;
    ca.async_wait_open([&](error_code ec) {
        if (!BOOST_TEST(!ec)) {
            return;
        }
        BOOST_TEST(!ca.send_text("one"));
        BOOST_TEST(!ca.send_text("two"));
        const std::string three = "three";
        BOOST_TEST(!ca.send(asio::buffer(three)));
        later.expires_after(unread);
        later.async_wait([&](error_code) { accept_and_read(p, cb, got, 3); });
    });
    p.negotiate([](error_code ec) { BOOST_TEST(!ec); });
    p.run();
    BOOST_TEST(!got.end);
    read_exactly(got, {"one", "two", "three"});
    BOOST_TEST((got.texts == std::vector<bool>{true, true, false}));
}

// Incoming channels past options::max_queued_channels, while no accept is pending, are closed at
// once, as a connection past a listen backlog is refused; the one kept is accepted later.
void a_channel_past_the_backlog() {
    webrtc::options one;
    one.max_queued_channels = 1;
    page_pair p(webrtc::options(), one);
    auto first = p.a->create_data_channel("first");
    auto second = p.a->create_data_channel("second");
    webcpp::test::require(BOOST_TEST(first.has_value()));
    webcpp::test::require(BOOST_TEST(second.has_value()));
    asio::steady_timer later(p.io);  // lint-world: accepts once both channels arrived
    std::optional<webrtc::data_channel> accepted;
    std::optional<std::string> refused;
    // The first channel to close is the one refused; the other closes with the connection.
    const auto watch = [&](webrtc::data_channel& channel) {
        channel.async_wait_close([&](error_code ec) {
            if (!ec && !refused && !accepted) {
                refused.emplace(channel.label());
            }
        });
    };
    watch(*first);
    watch(*second);
    // The refused channel may open before it closes, or close before its opening is seen, so
    // either end of each open wait will do.
    const auto settled = after(2, [&] {
        later.expires_after(unread);
        later.async_wait([&](error_code) {
            p.b->async_accept_data_channel([&](error_code ec, webrtc::data_channel dc) {
                if (!BOOST_TEST(!ec)) {
                    return;
                }
                accepted.emplace(std::move(dc));
                p.a->close();
                p.b->close();
            });
        });
    });
    first->async_wait_open([&](error_code) { settled(); });
    second->async_wait_open([&](error_code) { settled(); });
    p.negotiate([](error_code ec) { BOOST_TEST(!ec); });
    p.run();
    webcpp::test::require(BOOST_TEST(accepted.has_value()));
    webcpp::test::require(BOOST_TEST(refused.has_value()));
    BOOST_TEST(accepted->label() == "first" || accepted->label() == "second");
    BOOST_TEST(*refused != accepted->label());
}

}  // namespace

int main() {
    unread_messages_past_the_budget();
    a_received_message_past_the_size();
    refused_sends_leave_it_open();
    the_buffered_amount_wait();
    the_peer_closes_its_channel();
    a_channel_destroyed();
    closed_before_it_opened();
    accepted_after_its_messages();
    a_channel_past_the_backlog();
    return webcpp::test::finish("browser webrtc channels: ok");
}
