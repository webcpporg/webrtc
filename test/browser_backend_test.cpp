// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The browser's peer connection and data channel (design.md, D5, D8, D9, D13; plan, Tasks 2.3 and
// 2.4), under node, with an RTCPeerConnection and an RTCDataChannel of the test's, whose events
// the test fires itself, so that each case is deterministic; what a real browser does is the
// browser lane's. The connection: states coalesce, candidates come in order with their members and
// the end of gathering once, the promises' settlements complete the description operations and
// their rejections arrive by name, and close() aborts what waits, delivers no state change and
// closes the channels. The channel: its attributes, as created and as announced, its refusals to
// send leave it open, the buffered-amount wait, messages kept before the first receive, a message
// past max_message_size and the receive budget close it, its owner's close, the peer's close, and
// a channel accepted after its messages arrived, past the backlog closed at once.

#include <webcpp/webrtc/data_channel.hpp>
#include <webcpp/webrtc/peer_connection.hpp>
#include <webcpp/webrtc/runtime.hpp>

#include <webcpp/asio/bind_cancellation_slot.hpp>  // lint-world: an operation's cancellation
#include <webcpp/asio/buffer.hpp>                  // lint-world: a receive's dynamic buffer
#include <webcpp/asio/cancellation_signal.hpp>     // lint-world: an operation's cancellation
#include <webcpp/asio/error.hpp>                   // lint-world: Asio's codes
#include <webcpp/asio/io_context.hpp>              // lint-world: the test's event loop

#include <webcpp/test/exit_status.hpp>

#include <emscripten/em_js.h>
#include <emscripten/val.h>
#include <boost/core/lightweight_test.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;
using boost::system::error_code;
using emscripten::val;

// clang-format off
// The test's RTCPeerConnection, which keeps every instance in `connections`, and its
// RTCDataChannel, whose attributes are those of its RTCDataChannelInit, as a browser's are;
// `fire(target, handler, event)` calls a handler as the browser would, and `holding` keeps the
// connection's promises unsettled until `releaseHeld()`.
EM_JS(void, test_install_fakes, (), {
    class FakeChannel {
        constructor(label, init) {
            this.label = label;
            this.init = init;
            this.ordered = init?.ordered ?? true;
            this.maxPacketLifeTime = init?.maxPacketLifeTime ?? null;
            this.maxRetransmits = init?.maxRetransmits ?? null;
            this.protocol = init?.protocol ?? "";
            this.negotiated = init?.negotiated ?? false;
            this.readyState = 'connecting';
            this.bufferedAmount = 0;
            this.bufferedAmountLowThreshold = 0;
            this.id = init && Number.isInteger(init.id) ? init.id : null;
            this.binaryType = 'blob';
            this.sent = [];
            this.closed = false;
        }
        send(data) {
            if (this.readyState !== 'open') throw new DOMException('not open', 'InvalidStateError');
            this.sent.push(data);
        }
        close() {
            this.closed = true;
            if (this.readyState === 'closed') return;
            this.readyState = 'closing';
            setTimeout(() => {
                this.readyState = 'closed';
                if (this.onclose) this.onclose();
            }, 0);
        }
    }
    globalThis.FakeChannel = FakeChannel;
    // While `holding`, the connection's promises wait for releaseHeld(), which the test calls.
    globalThis.holding = false;
    globalThis.held = [];
    const later = action => {
        if (!holding) return Promise.resolve().then(action);
        return new Promise((resolve, reject) => {
            held.push(() => {
                try {
                    resolve(action());
                } catch (error) {
                    reject(error);
                }
            });
        });
    };
    globalThis.releaseHeld = () => {
        const waiting = held;
        held = [];
        for (const release of waiting) release();
    };
    globalThis.connections = [];
    globalThis.RTCPeerConnection = class {
        constructor(configuration) {
            this.configuration = configuration;
            this.connectionState = 'new';
            this.iceConnectionState = 'new';
            this.localDescription = null;
            this.remoteDescription = null;
            this.sctp = {maxMessageSize: 65536};
            this.channels = [];
            this.candidates = [];
            connections.push(this);
        }
        setLocalDescription() {
            return later(() => {
                this.localDescription = {type: this.remoteDescription ? 'answer' : 'offer', sdp: 'v=0 local'};
            });
        }
        setRemoteDescription(description) {
            return later(() => {
                if (description.sdp === 'bad') throw new DOMException('cannot parse', 'OperationError');
                this.remoteDescription = description;
            });
        }
        addIceCandidate(candidate) {
            return later(() => {
                if (!this.remoteDescription) throw new DOMException('no remote', 'InvalidStateError');
                this.candidates.push(candidate);
            });
        }
        createDataChannel(label, init) {
            const channel = new FakeChannel(label, init);
            this.channels.push(channel);
            return channel;
        }
        close() {
            this.connectionState = 'closed';
            for (const channel of this.channels) channel.readyState = 'closed';
        }
    };
    globalThis.fire = (target, handler, event) => {
        if (target[handler]) target[handler](event);
    };
    globalThis.lastConnection = () => connections[connections.length - 1];
    globalThis.bytes = (size, value) => new Uint8Array(size).fill(value).buffer;
});

// Returns to the event loop until its next task, without running the io_context: what the browser
// delivers meanwhile is staged, one batch for the next notification.
EM_ASYNC_JS(void, test_tick, (), {
    await new Promise(resolve => setTimeout(resolve, 0));
});
// clang-format on

namespace {

using connection = webrtc::peer_connection;
using channel = webrtc::data_channel;

// No ICE server: host candidates only.
const webrtc::configuration no_servers;

// Runs the posted completions and what the browser's promises settle, up to five seconds.
void settle(asio::io_context& io) {
    io.restart();
    io.run_for(std::chrono::seconds(5));  // lint-run: waits for the test's promises to settle
}

val last_connection() {
    return val::global("lastConnection")();
}

void fire(const val& target, const char* handler, const val& event = val::object()) {
    val::global("fire")(target, val(handler), event);
}

val bytes(int size, int value) {
    return val::global("bytes")(val(size), val(value));
}

val message(const val& data) {
    val event = val::object();
    event.set("data", data);
    return event;
}

void set_state(val pc, const char* name, const char* value, const char* handler) {
    pc.set(name, val(value));
    fire(pc, handler);
}

// States coalesce while no wait is pending, candidates come in order with every member, an empty
// candidate is no event, and the end of gathering comes once.
void connection_events() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made = connection::make(rt, io.get_executor(), no_servers);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    connection& pc = *made;
    val fake = last_connection();
    BOOST_TEST(fake["configuration"]["iceTransportPolicy"].strictlyEquals(val("all")));
    set_state(fake, "connectionState", "connecting", "onconnectionstatechange");
    set_state(fake, "connectionState", "connected", "onconnectionstatechange");
    set_state(fake, "iceConnectionState", "checking", "oniceconnectionstatechange");
    val candidate = val::object();
    candidate.set("candidate", val("candidate:1 1 udp 1 127.0.0.1 5000 typ host"));
    candidate.set("sdpMid", val("0"));
    candidate.set("sdpMLineIndex", val(0));
    candidate.set("usernameFragment", val("ufrag"));
    val event = val::object();
    event.set("candidate", candidate);
    fire(fake, "onicecandidate", event);
    val empty = val::object();
    empty.set("candidate", val(""));
    val generation_end = val::object();
    generation_end.set("candidate", empty);
    fire(fake, "onicecandidate", generation_end);
    val end = val::object();
    end.set("candidate", val::null());
    fire(fake, "onicecandidate", end);

    std::optional<webrtc::peer_connection_state> state;
    std::optional<webrtc::ice_connection_state> ice;
    std::vector<connection::ice_candidate> candidates;
    pc.async_wait_state_change([&](error_code ec, webrtc::peer_connection_state s) {
        BOOST_TEST(!ec);
        state = s;
    });
    pc.async_wait_ice_connection_state_change([&](error_code ec, webrtc::ice_connection_state s) {
        BOOST_TEST(!ec);
        ice = s;
    });
    for (int i = 0; i < 2; ++i) {
        pc.async_wait_ice_candidate([&](error_code ec, connection::ice_candidate c) {
            BOOST_TEST(!ec);
            candidates.push_back(std::move(c));
        });
        settle(io);
    }
    BOOST_TEST(state == webrtc::peer_connection_state::connected);
    BOOST_TEST(ice == webrtc::ice_connection_state::checking);
    BOOST_TEST(pc.state() == webrtc::peer_connection_state::connected);
    if (BOOST_TEST_EQ(candidates.size(), 2u)) {
        BOOST_TEST_EQ(candidates[0].candidate, "candidate:1 1 udp 1 127.0.0.1 5000 typ host");
        BOOST_TEST_EQ(candidates[0].sdp_mid, "0");
        BOOST_TEST(candidates[0].sdp_m_line_index == 0);
        BOOST_TEST_EQ(candidates[0].username_fragment, "ufrag");
        BOOST_TEST(candidates[1].candidate.empty());
    }
}

// The description operations follow the browser's promises: a resolution completes them, a
// rejection arrives by its name, and a candidate before a remote description is refused by the
// browser's own check.
void descriptions_follow_promises() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made = connection::make(rt, io.get_executor(), no_servers);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    connection& pc = *made;
    std::optional<error_code> early;
    pc.async_add_ice_candidate({.candidate = "candidate:2", .sdp_mid = "0"},
                               [&](error_code ec) { early = ec; });
    // The end of the remote candidates before a remote description is refused too, as natively.
    const webrtc::ice_candidate_view end_of_candidates;
    std::optional<error_code> early_end;
    pc.async_add_ice_candidate(end_of_candidates, [&](error_code ec) { early_end = ec; });
    std::optional<connection::session_description> local;
    pc.async_set_local_description([&](error_code ec, connection::session_description d) {
        BOOST_TEST(!ec);
        local = std::move(d);
    });
    std::optional<error_code> refused;
    pc.async_set_remote_description({.type = webrtc::sdp_type::answer, .sdp = "bad"},
                                    [&](error_code ec) { refused = ec; });
    // An end that waits for that refused description is refused with it.
    std::optional<error_code> late_end;
    pc.async_add_ice_candidate(end_of_candidates, [&](error_code ec) { late_end = ec; });
    settle(io);
    BOOST_TEST(early && *early == webrtc::error::invalid_state);
    BOOST_TEST(early_end && *early_end == webrtc::error::invalid_state);
    BOOST_TEST(late_end && *late_end == webrtc::error::invalid_state);
    BOOST_TEST(local && local->type == webrtc::sdp_type::offer && local->sdp == "v=0 local");
    BOOST_TEST(pc.local_description() && pc.local_description()->sdp == "v=0 local");
    BOOST_TEST(refused && *refused == webrtc::error::operation_error);
    BOOST_TEST(!pc.has_remote_description());
}

// Once a remote description is applied, a candidate is added with every member, and an empty one
// adds none and completes at once.
void candidates_follow_promises() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made = connection::make(rt, io.get_executor(), no_servers);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    connection& pc = *made;
    std::optional<error_code> applied;
    pc.async_set_remote_description({.type = webrtc::sdp_type::answer, .sdp = "v=0 remote"},
                                    [&](error_code ec) { applied = ec; });
    std::optional<error_code> added;
    pc.async_add_ice_candidate(
        {
            .candidate = "candidate:3",
            .sdp_mid = "0",
            .sdp_m_line_index = 0,
            .username_fragment = "u",
        },
        [&](error_code ec) { added = ec; });
    const webrtc::ice_candidate_view end_of_candidates;
    std::optional<error_code> ended;
    pc.async_add_ice_candidate(end_of_candidates, [&](error_code ec) { ended = ec; });
    settle(io);
    BOOST_TEST(applied && !*applied);
    BOOST_TEST(pc.has_remote_description());
    BOOST_TEST(added && !*added);
    BOOST_TEST(ended && !*ended);
    const val candidates = last_connection()["candidates"];
    if (BOOST_TEST_EQ(candidates["length"].as<int>(), 1)) {
        BOOST_TEST(candidates[0]["candidate"].strictlyEquals(val("candidate:3")));
        BOOST_TEST(candidates[0]["sdpMid"].strictlyEquals(val("0")));
        BOOST_TEST_EQ(candidates[0]["sdpMLineIndex"].as<int>(), 0);
        BOOST_TEST(candidates[0]["usernameFragment"].strictlyEquals(val("u")));
    }
}

// Arrivals of different kinds keep their order: a description's settlement the browser delivered
// before a candidate, in one batch, completes its operation first, so that a program that sends
// each candidate as it comes sends its offer before the offer's candidates (design.md, D8).
void arrivals_keep_their_order() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made = connection::make(rt, io.get_executor(), no_servers);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    connection& pc = *made;
    std::vector<std::string> order;
    pc.async_wait_ice_candidate([&](error_code ec, const connection::ice_candidate&) {
        if (!ec) {
            order.emplace_back("candidate");
        }
    });
    pc.async_set_local_description([&](error_code ec, const connection::session_description&) {
        if (!ec) {
            order.emplace_back("offer");
        }
    });
    test_tick();
    val candidate = val::object();
    candidate.set("candidate", val("candidate:1 1 udp 1 127.0.0.1 5000 typ host"));
    candidate.set("sdpMid", val("0"));
    val event = val::object();
    event.set("candidate", candidate);
    fire(last_connection(), "onicecandidate", event);
    settle(io);
    BOOST_TEST((order == std::vector<std::string>{"offer", "candidate"}));
}

// A close while a notification is in flight: what the browser staged before it is delivered to
// nothing, the waits end with operation_aborted, and the state reads closed (design.md, D8, D13).
void a_close_while_a_notification_is_in_flight() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made = connection::make(rt, io.get_executor(), no_servers);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    connection& pc = *made;
    std::optional<error_code> candidate;
    pc.async_wait_ice_candidate(
        [&](error_code ec, const connection::ice_candidate&) { candidate = ec; });
    const val fake = last_connection();
    set_state(fake, "connectionState", "connected", "onconnectionstatechange");
    val end = val::object();
    end.set("candidate", val::null());
    fire(fake, "onicecandidate", end);
    pc.close();
    settle(io);
    BOOST_TEST(candidate && *candidate == asio::error::operation_aborted);
    BOOST_TEST(pc.state() == webrtc::peer_connection_state::closed);
}

// A description's operation and a candidate's support terminal cancellation alone in the browser:
// each completes with operation_aborted, the browser's promise still settles, and what it applied
// stays applied, but its settlement finds no operation (design.md, D11).
void cancelled_promises() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made = connection::make(rt, io.get_executor(), no_servers);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    connection& pc = *made;
    val::global().set("holding", val(true));
    std::array<asio::cancellation_signal, 4> signals;
    std::array<std::vector<error_code>, 4> results;
    pc.async_set_local_description(asio::bind_cancellation_slot(
        signals[0].slot(),
        [&](error_code ec, const connection::session_description&) { results[0].push_back(ec); }));
    pc.async_set_remote_description(
        {.type = webrtc::sdp_type::offer, .sdp = "v=0 remote"},
        asio::bind_cancellation_slot(signals[1].slot(),
                                     [&](error_code ec) { results[1].push_back(ec); }));
    pc.async_add_ice_candidate({.candidate = "candidate:4", .sdp_mid = "0"},
                               asio::bind_cancellation_slot(signals[2].slot(), [&](error_code ec) {
                                   results[2].push_back(ec);
                               }));
    pc.async_add_ice_candidate({.candidate = "candidate:5", .sdp_mid = "0"},
                               asio::bind_cancellation_slot(signals[3].slot(), [&](error_code ec) {
                                   results[3].push_back(ec);
                               }));
    for (std::size_t i = 0; i < 3; ++i) {
        signals.at(i).emit(asio::cancellation_type::terminal);
    }
    // A partial cancellation is not supported, and changes nothing.
    signals[3].emit(asio::cancellation_type::partial);
    settle(io);
    for (std::size_t i = 0; i < 3; ++i) {
        BOOST_TEST((results.at(i) == std::vector<error_code>{asio::error::operation_aborted}));
    }
    BOOST_TEST(results[3].empty());
    val::global().set("holding", val(false));
    val::global("releaseHeld")();
    settle(io);
    for (std::size_t i = 0; i < 3; ++i) {
        BOOST_TEST_EQ(results.at(i).size(), 1u);
    }
    BOOST_TEST((results[3] == std::vector<error_code>{error_code{}}));
    BOOST_TEST(pc.has_remote_description());
    BOOST_TEST(pc.local_description().has_value());
}

// A channel's attributes are the browser's: a created channel's are those it was created with,
// and an accepted channel's those the peer announced, never negotiated.
void a_channels_attributes() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made = connection::make(rt, io.get_executor(), no_servers);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    connection& pc = *made;
    const webrtc::data_channel_init made_with{
        .ordered = false,
        .max_packet_life_time = std::nullopt,
        .max_retransmits = 4,
        .protocol = "made.v1",
        .negotiated = true,
        .id = 9,
    };
    auto created = pc.create_data_channel("made", made_with);
    if (!BOOST_TEST(created.has_value())) {
        return;
    }
    BOOST_TEST(!created->ordered());
    BOOST_TEST(!created->max_packet_life_time());
    BOOST_TEST(created->max_retransmits() == std::optional<std::uint16_t>(4));
    BOOST_TEST_EQ(created->protocol(), "made.v1");
    BOOST_TEST(created->negotiated());
    auto reliable = pc.create_data_channel("reliable");
    if (!BOOST_TEST(reliable.has_value())) {
        return;
    }
    BOOST_TEST(reliable->ordered());
    BOOST_TEST(!reliable->max_packet_life_time());
    BOOST_TEST(!reliable->max_retransmits());
    BOOST_TEST(reliable->protocol().empty());
    BOOST_TEST(!reliable->negotiated());

    val init = val::object();
    init.set("ordered", val(false));
    init.set("maxPacketLifeTime", val(250));
    init.set("protocol", val("announced.v1"));
    val incoming = val::global("FakeChannel").new_(val("incoming"), init);
    incoming.set("readyState", val("open"));
    val announced = val::object();
    announced.set("channel", incoming);
    fire(last_connection(), "ondatachannel", announced);
    std::optional<channel> accepted;
    pc.async_accept_data_channel([&](error_code ec, channel c) {
        BOOST_TEST(!ec);
        accepted.emplace(std::move(c));
    });
    settle(io);
    if (!BOOST_TEST(accepted.has_value())) {
        return;
    }
    BOOST_TEST(!accepted->ordered());
    BOOST_TEST(accepted->max_packet_life_time() == std::optional<std::uint16_t>(250));
    BOOST_TEST(!accepted->max_retransmits());
    BOOST_TEST_EQ(accepted->protocol(), "announced.v1");
    BOOST_TEST(!accepted->negotiated());
}

// A send is refused, and the channel stays open, when it is not open, past max_message_size or
// the peer's limit, and past max_buffered_amount; the buffered-amount wait completes with the
// browser's event once the amount is at the threshold.
void sends_and_their_refusals() {
    asio::io_context io;
    webrtc::runtime rt;
    webrtc::options opts;
    opts.max_message_size = 16;
    opts.max_buffered_amount = 64;
    auto made = connection::make(rt, io.get_executor(), no_servers, opts);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    auto created =
        made->create_data_channel("chat", {.protocol = "p", .negotiated = true, .id = 3});
    if (!BOOST_TEST(created.has_value())) {
        return;
    }
    channel& dc = *created;
    val fake = last_connection()["channels"][0];
    BOOST_TEST_EQ(dc.label(), "chat");
    BOOST_TEST(dc.id() == 3);
    BOOST_TEST(fake["binaryType"].strictlyEquals(val("arraybuffer")));
    BOOST_TEST(fake["init"]["protocol"].strictlyEquals(val("p")));
    BOOST_TEST(dc.ready_state() == webrtc::data_channel_state::connecting);
    const std::array<char, 4> ping = {'p', 'i', 'n', 'g'};
    BOOST_TEST(dc.send(asio::buffer(ping)) == webrtc::error::invalid_state);

    bool opened = false;
    dc.async_wait_open([&](error_code ec) { opened = !ec; });
    fake.set("readyState", val("open"));
    fire(fake, "onopen");
    settle(io);
    BOOST_TEST(opened);
    BOOST_TEST(dc.ready_state() == webrtc::data_channel_state::open);
    BOOST_TEST(!dc.send(asio::buffer(ping)));
    BOOST_TEST(!dc.send_text("pong"));
    BOOST_TEST_EQ(fake["sent"]["length"].as<int>(), 2);
    BOOST_TEST(fake["sent"][1].strictlyEquals(val("pong")));

    const std::string seventeen(17, 'x');
    BOOST_TEST(dc.send_text(seventeen) == webrtc::error::message_too_large);
    last_connection()["sctp"].set("maxMessageSize", val(6));
    BOOST_TEST(dc.send_text("seven!!") == webrtc::error::message_too_large);
    last_connection()["sctp"].set("maxMessageSize", val(65536));
    // 60 queued and 4 sent make the bound exactly; one byte more passes it.
    fake.set("bufferedAmount", val(60));
    BOOST_TEST(!dc.send(asio::buffer(ping)));
    fake.set("bufferedAmount", val(61));
    BOOST_TEST(dc.send(asio::buffer(ping)) == webrtc::error::buffered_amount_exceeded);
    BOOST_TEST(!fake["closed"].as<bool>());
    BOOST_TEST(dc.ready_state() == webrtc::data_channel_state::open);

    dc.set_buffered_amount_low_threshold(10);
    BOOST_TEST_EQ(fake["bufferedAmountLowThreshold"].as<int>(), 10);
    bool low = false;
    dc.async_wait_buffered_amount_low([&](error_code ec) { low = !ec; });
    settle(io);
    BOOST_TEST(!low);
    fake.set("bufferedAmount", val(10));
    fire(fake, "onbufferedamountlow");
    settle(io);
    BOOST_TEST(low);
    BOOST_TEST_EQ(dc.buffered_amount(), 10u);
}

// Messages are kept from the channel's creation: binary and text, in order, the text with
// is_text; a message larger than the room left in the buffer stays queued.
void messages_and_the_buffer() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made = connection::make(rt, io.get_executor(), no_servers);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    auto created = made->create_data_channel("data");
    if (!BOOST_TEST(created.has_value())) {
        return;
    }
    channel& dc = *created;
    val fake = last_connection()["channels"][0];
    fake.set("readyState", val("open"));
    fire(fake, "onopen");
    fire(fake, "onmessage", message(bytes(3, 7)));
    fire(fake, "onmessage", message(val::u8string("h\xc3\xa9")));
    std::string small;
    auto limited = asio::dynamic_buffer(small, 2);
    std::optional<error_code> refused;
    dc.async_receive(limited, [&](error_code ec, std::size_t, bool) { refused = ec; });
    settle(io);
    BOOST_TEST(refused && *refused == asio::error::no_buffer_space);
    std::string data;
    auto buffer = asio::dynamic_buffer(data);
    std::vector<std::pair<std::size_t, bool>> got;
    for (int i = 0; i < 2; ++i) {
        dc.async_receive(buffer, [&](error_code ec, std::size_t size, bool text) {
            BOOST_TEST(!ec);
            got.emplace_back(size, text);
        });
        settle(io);
    }
    if (BOOST_TEST_EQ(got.size(), 2u)) {
        BOOST_TEST(got[0] == std::make_pair(std::size_t{3}, false));
        BOOST_TEST(got[1] == std::make_pair(std::size_t{3}, true));
    }
    BOOST_TEST_EQ(data, std::string("\x07\x07\x07h\xc3\xa9"));
}

// Past the receive budget, a message's payload and record counted, or past max_message_size, the
// channel closes at once and receives fail with the cause; what was queued is discarded.
void the_budget_or_the_size(bool oversized) {
    using state =
        webrtc::detail::browser::channel_state<asio::any_io_executor, std::allocator<std::byte>>;
    asio::io_context io;
    webrtc::runtime rt;
    webrtc::options opts;
    opts.max_message_size = 16;
    opts.max_received_bytes = (2 * (10 + state::message_record_bytes)) - 1;
    auto made = connection::make(rt, io.get_executor(), no_servers, opts);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    auto created = made->create_data_channel("data");
    if (!BOOST_TEST(created.has_value())) {
        return;
    }
    channel& dc = *created;
    val fake = last_connection()["channels"][0];
    fake.set("readyState", val("open"));
    fire(fake, "onopen");
    fire(fake, "onmessage", message(bytes(10, 1)));
    BOOST_TEST(!fake["closed"].as<bool>());
    fire(fake, "onmessage", message(bytes(oversized ? 17 : 10, 2)));
    BOOST_TEST(fake["closed"].as<bool>());
    const auto cause =
        oversized ? webrtc::error::message_too_large : webrtc::error::receive_overflow;
    std::string data;
    auto buffer = asio::dynamic_buffer(data);
    std::optional<error_code> received;
    dc.async_receive(buffer, [&](error_code ec, std::size_t, bool) { received = ec; });
    bool closed = false;
    dc.async_wait_close([&](error_code ec) { closed = !ec; });
    settle(io);
    BOOST_TEST(received && *received == cause);
    BOOST_TEST(data.empty());
    BOOST_TEST(closed);
    BOOST_TEST(dc.ready_state() == webrtc::data_channel_state::closed);
}

// The owner's close aborts what waits but async_wait_close, which completes once the browser
// closed the channel.
void the_owners_close() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made = connection::make(rt, io.get_executor(), no_servers);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    auto created = made->create_data_channel("owner");
    if (!BOOST_TEST(created.has_value())) {
        return;
    }
    channel& dc = *created;
    std::optional<error_code> open;
    std::optional<error_code> closed;
    dc.async_wait_open([&](error_code ec) { open = ec; });
    dc.async_wait_close([&](error_code ec) { closed = ec; });
    dc.close();
    settle(io);
    BOOST_TEST(open && *open == asio::error::operation_aborted);
    BOOST_TEST(closed && !*closed);
    BOOST_TEST(dc.ready_state() == webrtc::data_channel_state::closed);
}

// The peer's close delivers what came before, then error::closed.
void the_peers_close() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made = connection::make(rt, io.get_executor(), no_servers);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    auto created = made->create_data_channel("peer");
    if (!BOOST_TEST(created.has_value())) {
        return;
    }
    channel& dc = *created;
    val peer = last_connection()["channels"][0];
    peer.set("readyState", val("open"));
    fire(peer, "onopen");
    fire(peer, "onmessage", message(val("last")));
    peer.set("readyState", val("closed"));
    fire(peer, "onclose");
    std::string data;
    auto buffer = asio::dynamic_buffer(data);
    std::vector<error_code> receives;
    for (int i = 0; i < 2; ++i) {
        dc.async_receive(buffer, [&](error_code ec, std::size_t, bool) { receives.push_back(ec); });
        settle(io);
    }
    if (BOOST_TEST_EQ(receives.size(), 2u)) {
        BOOST_TEST(!receives[0]);
        BOOST_TEST(receives[1] == webrtc::error::closed);
    }
    BOOST_TEST_EQ(data, "last");
}

// The connection's close ends the channel's waits with error::closed and its own with
// operation_aborted, delivers no state change, sets its handlers to null, and refuses what comes
// after.
void the_connections_close() {
    asio::io_context io;
    webrtc::runtime rt;
    auto made = connection::make(rt, io.get_executor(), no_servers);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    connection& pc = *made;
    auto created = pc.create_data_channel("connection");
    if (!BOOST_TEST(created.has_value())) {
        return;
    }
    channel& dc = *created;
    std::optional<error_code> state;
    pc.async_wait_state_change([&](error_code ec, webrtc::peer_connection_state) { state = ec; });
    std::string data;
    auto buffer = asio::dynamic_buffer(data);
    std::optional<error_code> received;
    dc.async_receive(buffer, [&](error_code ec, std::size_t, bool) { received = ec; });
    std::optional<error_code> closed;
    dc.async_wait_close([&](error_code ec) { closed = ec; });
    pc.close();
    settle(io);
    BOOST_TEST(state && *state == asio::error::operation_aborted);
    BOOST_TEST(pc.state() == webrtc::peer_connection_state::closed);
    BOOST_TEST(received && *received == webrtc::error::closed);
    BOOST_TEST(closed && !*closed);
    BOOST_TEST(pc.create_data_channel("late").error() == webrtc::error::invalid_state);
    std::optional<error_code> later;
    pc.async_wait_ice_candidate(
        [&](error_code ec, const connection::ice_candidate&) { later = ec; });
    settle(io);
    BOOST_TEST(later && *later == webrtc::error::closed);
    BOOST_TEST(last_connection()["onicecandidate"].isNull());
}

// A channel the peer opened is taken over at once: the messages it receives before it is
// accepted are kept. One past the backlog is closed at once; destroying a channel aborts its
// receive and sets its handlers to null.
void a_channel_accepted_late() {
    asio::io_context io;
    webrtc::runtime rt;
    webrtc::options opts;
    opts.max_queued_channels = 1;
    auto made = connection::make(rt, io.get_executor(), no_servers, opts);
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    connection& pc = *made;
    val fake = last_connection();
    const val channel_class = val::global("FakeChannel");
    val incoming = channel_class.new_(val("incoming"), val::object());
    val refused = channel_class.new_(val("refused"), val::object());
    incoming.set("readyState", val("open"));
    refused.set("readyState", val("open"));
    val announced = val::object();
    announced.set("channel", incoming);
    fire(fake, "ondatachannel", announced);
    fire(incoming, "onmessage", message(val("one")));
    fire(incoming, "onmessage", message(val("two")));
    val past = val::object();
    past.set("channel", refused);
    fire(fake, "ondatachannel", past);
    BOOST_TEST(refused["closed"].as<bool>());
    settle(io);

    std::optional<channel> accepted;
    pc.async_accept_data_channel([&](error_code ec, channel c) {
        BOOST_TEST(!ec);
        accepted.emplace(std::move(c));
    });
    settle(io);
    if (!BOOST_TEST(accepted.has_value())) {
        return;
    }
    BOOST_TEST_EQ(accepted->label(), "incoming");
    BOOST_TEST(accepted->ready_state() == webrtc::data_channel_state::open);
    std::string data;
    auto buffer = asio::dynamic_buffer(data);
    for (int i = 0; i < 2; ++i) {
        accepted->async_receive(buffer, [&](error_code ec, std::size_t, bool text) {
            BOOST_TEST(!ec);
            BOOST_TEST(text);
        });
        settle(io);
    }
    BOOST_TEST_EQ(data, "onetwo");

    std::optional<error_code> aborted;
    accepted->async_receive(buffer, [&](error_code ec, std::size_t, bool) { aborted = ec; });
    accepted.reset();
    settle(io);
    BOOST_TEST(aborted && *aborted == asio::error::operation_aborted);
    BOOST_TEST(incoming["onmessage"].isNull());
    BOOST_TEST(incoming["closed"].as<bool>());
}

// An allocator that poisons what it frees and never gives it again, so that a state used after it
// was freed reads poison and traps, rather than reading what it held.
template <class T>
struct poisoning_allocator {
    using value_type = T;

    poisoning_allocator() = default;

    template <class U>
    explicit poisoning_allocator(const poisoning_allocator<U>& /*other*/) noexcept {}

    T* allocate(std::size_t n) { return std::allocator<T>().allocate(n); }

    // Kept, never freed: the test's few states stay poisoned until the program ends.
    void deallocate(T* p, std::size_t n) noexcept {
        std::memset(static_cast<void*>(p), 0xDD, n * sizeof(T));
    }

    template <class U>
    bool operator==(const poisoning_allocator<U>& /*other*/) const noexcept {
        return true;
    }
};

using poisoned_connection =
    webrtc::basic_peer_connection<asio::any_io_executor, poisoning_allocator<std::byte>>;
using poisoned_channel =
    webrtc::basic_data_channel<asio::any_io_executor, poisoning_allocator<std::byte>>;

// The usual Asio session: it owns its connection and its channel, and every pending handler holds
// it, so that it lives as long as something waits.
class session : public std::enable_shared_from_this<session> {
public:
    session(poisoned_connection pc, poisoned_channel dc, bool& destroyed, bool& called)
        : pc_(std::move(pc)), dc_(std::move(dc)), destroyed_(destroyed), called_(called) {}

    session(const session&) = delete;
    session& operator=(const session&) = delete;
    session(session&&) = delete;
    session& operator=(session&&) = delete;

    ~session() { destroyed_ = true; }

    // Waits on the channel alone.
    void wait_on_the_channel() {
        dc_.async_wait_open([self = shared_from_this()](error_code) { self->called_ = true; });
        dc_.async_receive(buffer_, [self = shared_from_this()](error_code, std::size_t, bool) {
            self->called_ = true;
        });
    }

    // Waits on the connection alone, a description in flight among them.
    void wait_on_the_connection() {
        pc_.async_wait_state_change(
            [self = shared_from_this()](error_code, webrtc::peer_connection_state) {
                self->called_ = true;
            });
        pc_.async_wait_ice_candidate(
            [self = shared_from_this()](error_code, const poisoned_connection::ice_candidate&) {
                self->called_ = true;
            });
        pc_.async_set_local_description(
            [self = shared_from_this()](error_code,
                                        const poisoned_connection::session_description&) {
                self->called_ = true;
            });
    }

private:
    poisoned_connection pc_;
    poisoned_channel dc_;
    std::string data_;
    asio::dynamic_string_buffer<char, std::char_traits<char>, std::allocator<char>> buffer_ =
        asio::dynamic_buffer(data_);
    bool& destroyed_;
    bool& called_;
};

// The io_context is destroyed while handlers that own the connection and the channel wait: its
// shutdown destroys them without calling them, and the state they freed is not used afterwards
// (design.md, D8).
void shutdown_while_handlers_own_the_facades(bool on_the_channel) {
    webrtc::runtime rt;
    auto io = std::make_unique<asio::io_context>();
    bool destroyed = false;
    bool called = false;
    val::global().set("holding", val(true));
    {
        auto made = poisoned_connection::make(rt, io->get_executor(), no_servers);
        if (!BOOST_TEST(made.has_value())) {
            return;
        }
        auto created = made->create_data_channel("session");
        if (!BOOST_TEST(created.has_value())) {
            return;
        }
        const auto s =
            std::make_shared<session>(std::move(*made), std::move(*created), destroyed, called);
        if (on_the_channel) {
            s->wait_on_the_channel();
        } else {
            s->wait_on_the_connection();
        }
    }
    BOOST_TEST(!destroyed);
    io.reset();
    BOOST_TEST(destroyed);
    BOOST_TEST(!called);
    val::global().set("holding", val(false));
    val::global("releaseHeld")();
}

}  // namespace

int main() {
    test_install_fakes();
    connection_events();
    descriptions_follow_promises();
    candidates_follow_promises();
    arrivals_keep_their_order();
    a_close_while_a_notification_is_in_flight();
    cancelled_promises();
    a_channels_attributes();
    sends_and_their_refusals();
    messages_and_the_buffer();
    the_budget_or_the_size(false);
    the_budget_or_the_size(true);
    the_owners_close();
    the_peers_close();
    the_connections_close();
    a_channel_accepted_late();
    shutdown_while_handlers_own_the_facades(true);
    shutdown_while_handlers_own_the_facades(false);
    // The io_context's run_for suspends main, whose returned status Asyncify loses.
    return webcpp::test::exit_status(boost::report_errors());
}
