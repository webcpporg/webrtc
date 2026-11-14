// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The browser's imports (design.md, D8; plan, Task 2.1), under node, which has no WebRTC of its
// own: each import is given an object of the test's whose constructor, method or property throws
// a DOMException of each name webrtc knows, one it does not, a TypeError and a value that is no
// error. Every exception stays in JavaScript: an import that returns reports the name; an import
// that follows a promise delivers the name through its callback, later, when the promise settles,
// and never during its own call, so that C++ is never entered from an import; one that reads a
// value gives its default. The names become webrtc's errors as design.md's D10 says, a promise
// that resolves delivers what it resolved with, `make` counts its connection in the page's census,
// and `detach` sets every handler the imports set to null.

#include <webcpp/webrtc/detail/browser/census.hpp>
#include <webcpp/webrtc/detail/browser/imports.hpp>
#include <webcpp/webrtc/error.hpp>

#include <webcpp/asio/browser/bridge.hpp>       // lint-world: the browser's events
#include <webcpp/asio/executor_work_guard.hpp>  // lint-world: the test's event loop
#include <webcpp/asio/io_context.hpp>           // lint-world: the test's event loop
#include <webcpp/asio/post.hpp>                 // lint-world: the test's event loop
#include <webcpp/asio/steady_timer.hpp>         // lint-world: the test's deadline

#include <webcpp/test/exit_status.hpp>

#include <emscripten/em_js.h>
#include <emscripten/val.h>
#include <boost/core/lightweight_test.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The test's objects, on globalThis: `throwing(name)` throws a DOMException of that name, a
// TypeError for "TypeError" and a string, which has no name, for ""; `rejecting(name)` is a promise
// rejected with it; the connections and the objects whose methods, setters or getters throw it;
// and an RTCPeerConnection whose constructor throws `testName`, or one that is made.
// clang-format off
EM_JS(void, test_install_objects, (), {
    globalThis.throwing = name => {
        if (name === "") throw 'no name';
        if (name === 'TypeError') throw new TypeError('thrown by the test');
        throw new DOMException('thrown by the test', name);
    };
    globalThis.rejecting = name => new Promise(() => throwing(name));
    globalThis.useThrowingConstructor = () => {
        globalThis.RTCPeerConnection = class { constructor() { throwing(globalThis.testName); } };
    };
    globalThis.useClosingConstructor = () => {
        globalThis.RTCPeerConnection = class { close() {} };
    };
    globalThis.throwingConnection = name => {
        return {
            setLocalDescription() { throwing(name); },
            setRemoteDescription() { throwing(name); },
            addIceCandidate() { throwing(name); },
        };
    };
    globalThis.rejectingConnection = name => {
        return {
            setLocalDescription() { return rejecting(name); },
            setRemoteDescription() { return rejecting(name); },
            addIceCandidate() { return rejecting(name); },
        };
    };
    globalThis.resolvingConnection = {
        async setLocalDescription() { this.localDescription = {type: 'answer', sdp: 'v=0 local'}; },
        async setRemoteDescription(description) { this.remote = description; },
        async addIceCandidate(candidate) { this.candidate = candidate; },
    };
    globalThis.throwingObject = name => {
        const object = {
            createDataChannel() { throwing(name); },
            close() { throwing(name); },
            send() { throwing(name); },
        };
        for (const property of ['binaryType', 'onopen', 'onconnectionstatechange',
                                'bufferedAmountLowThreshold']) {
            Object.defineProperty(object, property, {set() { throwing(name); }});
        }
        return object;
    };
    globalThis.sink = {sent: [], send(data) { this.sent.push(data); }};
    globalThis.unreadable = {};
    for (const property of ['readyState', 'bufferedAmount', 'id', 'label', 'sctp',
                            'remoteDescription', 'localDescription']) {
        Object.defineProperty(unreadable, property, {get() { throwing('OperationError'); }});
    }
    globalThis.testBuffer = new Uint8Array([1, 2, 0, 255]).buffer;
});
// clang-format on

namespace asio = webcpp::asio;
namespace browser = webcpp::webrtc::detail::browser;
using boost::system::error_code;
using emscripten::val;
using webcpp::webrtc::error;

namespace {

// The names a test object throws: webrtc's five, one it does not know, and "" for a thrown value
// that has no name.
constexpr std::array<const char*, 7> names = {
    "InvalidStateError",
    "OperationError",
    "InvalidAccessError",
    "TypeError",
    "SyntaxError",
    "NotSupportedError",
    "",
};

// What each name becomes (design.md, D10).
constexpr std::array<error, 7> errors = {
    error::invalid_state, error::operation_error, error::invalid_access,  error::type_error,
    error::syntax_error,  error::operation_error, error::operation_error,
};

/** Records what the bridge delivers, and whether a delivery came during an import's call. */
class recorder final : public asio::browser::registration {
public:
    explicit recorder(asio::io_context& io) : io_(io) {}

    bool deliver(const val& event) override {
        if (inside) {
            ++reentered;
        }
        events.push_back(event);
        asio::post(io_, [this] {
            if (arrived) {
                arrived();
            }
        });
        return false;
    }

    void shutdown() noexcept override {}

    std::vector<val> events;
    bool inside = false;  // set by the test around each import's call
    int reentered = 0;
    std::function<void()> arrived;

private:
    asio::io_context& io_;
};

// Runs `io` until `r` holds `count` events, or ten seconds passed.
void wait_for(asio::io_context& io, recorder& r, std::size_t count) {
    if (r.events.size() >= count) {
        return;
    }
    auto guard = asio::make_work_guard(io);
    asio::steady_timer deadline(io, std::chrono::seconds(10));
    deadline.async_wait([&](error_code ec) {
        if (!ec) {
            guard.reset();
        }
    });
    r.arrived = [&] {
        if (r.events.size() >= count) {
            guard.reset();
            deadline.cancel();
        }
    };
    io.restart();
    io.run();  // lint-run: waits for the promises the browser's imports follow
    r.arrived = nullptr;
    BOOST_TEST_GE(r.events.size(), count);
}

bool is_name(const val& value, const char* name) {
    return value.isString() && value.strictlyEquals(val(name));
}

// Each name becomes the error design.md's D10 gives it, and null is no error.
void names_become_errors() {
    for (std::size_t i = 0; i < names.size(); ++i) {
        BOOST_TEST(browser::error_of(val(names.at(i))) == errors.at(i));
        BOOST_TEST(browser::outcome_of(val(names.at(i))) == errors.at(i));
    }
    BOOST_TEST(!browser::outcome_of(val::null()));
    BOOST_TEST(!browser::outcome_of(val::undefined()));
}

// make reports the constructor's exception, counts nothing then, and on success counts the
// connection and sets the four handlers, which detach sets to null.
void make_and_detach() {
    asio::io_context io;
    recorder r(io);
    auto& bridge = asio::use_service<asio::browser::bridge>(io);
    const std::uint64_t key = bridge.add(r);
    const val callback = bridge.function(key);
    const val census = browser::census();
    const std::size_t before = browser::census_held();
    val::global("useThrowingConstructor")();
    for (const char* name : names) {
        val::global().set("testName", val(name));
        r.inside = true;
        const val made = browser::take(webcpp_webrtc_make(
            val::object().as_handle(), callback.as_handle(), census.as_handle()));
        r.inside = false;
        BOOST_TEST(is_name(made["error"], name));
        BOOST_TEST(made["pc"].isUndefined());
    }
    BOOST_TEST_EQ(browser::census_held(), before);

    val::global("useClosingConstructor")();
    const val made = browser::take(
        webcpp_webrtc_make(val::object().as_handle(), callback.as_handle(), census.as_handle()));
    const val pc = made["pc"];
    BOOST_TEST(made["error"].isUndefined());
    BOOST_TEST_EQ(browser::census_held(), before + 1);
    const std::array<const char*, 4> handlers = {
        "onconnectionstatechange",
        "oniceconnectionstatechange",
        "onicecandidate",
        "ondatachannel",
    };
    for (const char* handler : handlers) {
        BOOST_TEST(pc[handler].typeOf().strictlyEquals(val("function")));
    }
    BOOST_TEST(browser::take(webcpp_webrtc_detach(pc.as_handle(), false)).isNull());
    for (const char* handler : handlers) {
        BOOST_TEST(pc[handler].isNull());
    }
    BOOST_TEST(r.events.empty());
    bridge.remove(key);
}

// One rejection the bridge delivered, of the `failures` the test started, `seen` marking it: three
// imports a connection, two connections a name, in the order of names.
void check_rejection(const val& event, std::size_t failures, std::vector<bool>& seen) {
    BOOST_TEST_EQ(browser::kind_of(event), static_cast<int>(browser::event_kind::settled));
    BOOST_TEST(!event["ok"].as<bool>());
    const auto which = static_cast<std::size_t>(event["id"].as<double>());
    if (!BOOST_TEST(which >= 1 && which <= failures)) {
        return;
    }
    seen.at(which) = true;
    BOOST_TEST(is_name(event["name"], names.at((which - 1) / 6)));
    BOOST_TEST_EQ(event["local"].as<bool>(), (which - 1) % 3 == 0);
}

// The three imports that follow a promise deliver the name of a synchronous exception and of a
// rejection alike, later, and never during their call.
void rejections_arrive_by_name() {
    asio::io_context io;
    recorder r(io);
    auto& bridge = asio::use_service<asio::browser::bridge>(io);
    const std::uint64_t key = bridge.add(r);
    const val callback = bridge.function(key);
    double id = 0;
    for (const char* name : names) {
        for (const char* make : {"throwingConnection", "rejectingConnection"}) {
            const val pc = val::global(make)(val(name));
            r.inside = true;
            webcpp_webrtc_set_local(pc.as_handle(), callback.as_handle(), ++id);
            webcpp_webrtc_set_remote(pc.as_handle(), 1, val("v=0").as_handle(),
                                     callback.as_handle(), ++id);
            webcpp_webrtc_add_candidate(pc.as_handle(), val::object().as_handle(),
                                        callback.as_handle(), ++id);
            r.inside = false;
        }
    }
    BOOST_TEST(r.events.empty());
    const auto failures = static_cast<std::size_t>(id);
    wait_for(io, r, failures);
    BOOST_TEST_EQ(r.reentered, 0);
    std::vector<bool> seen(failures + 1, false);
    for (const val& event : r.events) {
        check_rejection(event, failures, seen);
    }
    for (std::size_t which = 1; which <= failures; ++which) {
        BOOST_TEST(seen.at(which));
    }
    bridge.remove(key);
}

// A promise that resolves delivers its outcome, a local description its type and SDP, later.
void resolutions_deliver_their_outcome() {
    asio::io_context io;
    recorder r(io);
    auto& bridge = asio::use_service<asio::browser::bridge>(io);
    const std::uint64_t key = bridge.add(r);
    const val callback = bridge.function(key);
    const val pc = val::global("resolvingConnection");
    r.inside = true;
    webcpp_webrtc_set_local(pc.as_handle(), callback.as_handle(), 101);
    webcpp_webrtc_set_remote(pc.as_handle(), 0, val("v=0 remote").as_handle(), callback.as_handle(),
                             102);
    webcpp_webrtc_add_candidate(pc.as_handle(), val::object().as_handle(), callback.as_handle(),
                                103);
    r.inside = false;
    BOOST_TEST(r.events.empty());
    wait_for(io, r, 3);
    BOOST_TEST_EQ(r.reentered, 0);
    for (const val& event : r.events) {
        BOOST_TEST(event["ok"].as<bool>());
        if (event["id"].as<double>() == 101) {
            BOOST_TEST(event["local"].as<bool>());
            BOOST_TEST_EQ(event["type"].as<int>(), 1);
            BOOST_TEST(event["sdp"].strictlyEquals(val("v=0 local")));
        }
    }
    BOOST_TEST(pc["remote"]["type"].strictlyEquals(val("offer")));
    BOOST_TEST(pc["remote"]["sdp"].strictlyEquals(val("v=0 remote")));
    bridge.remove(key);
}

// Each import that returns reports the name of what the browser's object threw.
void returning_imports_report_names() {
    const std::array<char, 4> bytes = {'p', 'i', 'n', 'g'};
    for (const char* name : names) {
        const val object = val::global("throwingObject")(val(name));
        const val made = browser::take(webcpp_webrtc_channel(
            object.as_handle(), val("label").as_handle(), val::object().as_handle()));
        BOOST_TEST(is_name(made["error"], name));
        BOOST_TEST(made["channel"].isUndefined());
        const val attached =
            browser::take(webcpp_webrtc_attach(object.as_handle(), val::undefined().as_handle()));
        BOOST_TEST(is_name(attached, name));
        BOOST_TEST(is_name(browser::take(webcpp_webrtc_detach(object.as_handle(), true)), name));
        BOOST_TEST(is_name(browser::take(webcpp_webrtc_detach(object.as_handle(), false)), name));
        BOOST_TEST(is_name(browser::take(webcpp_webrtc_close(object.as_handle())), name));
        const val sent = browser::take(
            webcpp_webrtc_send(object.as_handle(), bytes.data(), bytes.size(), false));
        BOOST_TEST(is_name(sent, name));
        BOOST_TEST(
            is_name(browser::take(webcpp_webrtc_set_threshold(object.as_handle(), 1)), name));
    }
}

// A text that is not UTF-8 is refused with a TypeError, before the channel's send.
void a_text_must_be_utf8() {
    const val sink = val::global("sink");
    const std::array<char, 2> invalid = {'\xc3', '\x28'};
    const val refused =
        browser::take(webcpp_webrtc_send(sink.as_handle(), invalid.data(), invalid.size(), true));
    BOOST_TEST(is_name(refused, "TypeError"));
    const std::string text = "h\xc3\xa9";
    const val sent =
        browser::take(webcpp_webrtc_send(sink.as_handle(), text.data(), text.size(), true));
    BOOST_TEST(sent.isNull());
    BOOST_TEST_EQ(sink["sent"]["length"].as<int>(), 1);
    BOOST_TEST(sink["sent"][0].strictlyEquals(val::u8string("h\xc3\xa9")));
}

// An import that reads a value gives its default when the read throws.
void reads_give_their_default() {
    const val object = val::global("unreadable");
    BOOST_TEST_EQ(webcpp_webrtc_ready_state(object.as_handle()), 3);
    BOOST_TEST_EQ(webcpp_webrtc_buffered_amount(object.as_handle()), 0.0);
    BOOST_TEST_EQ(webcpp_webrtc_channel_id(object.as_handle()), -1);
    BOOST_TEST(browser::take(webcpp_webrtc_label(object.as_handle())).strictlyEquals(val("")));
    BOOST_TEST_EQ(webcpp_webrtc_peer_max_message_size(object.as_handle()), 0.0);
    BOOST_TEST(!webcpp_webrtc_has_remote(object.as_handle()));
    BOOST_TEST(browser::take(webcpp_webrtc_local_description(object.as_handle())).isNull());
}

// The texts and the bytes cross whole, a text as UTF-8.
void texts_and_bytes_cross() {
    const std::string text = "caf\xc3\xa9 \xe2\x82\xac";
    const val string = browser::js_string(text);
    BOOST_TEST_EQ(string["length"].as<int>(), 6);
    std::string back;
    browser::js_copy(string, back);
    BOOST_TEST_EQ(back, text);
    std::vector<unsigned char> bytes;
    browser::js_copy(val::global("testBuffer"), bytes);
    BOOST_TEST((bytes == std::vector<unsigned char>{1, 2, 0, 255}));
}

}  // namespace

int main() {
    test_install_objects();
    names_become_errors();
    make_and_detach();
    rejections_arrive_by_name();
    resolutions_deliver_their_outcome();
    returning_imports_report_names();
    a_text_must_be_utf8();
    reads_give_their_default();
    texts_and_bytes_cross();
    // The io_context's run suspends main, whose returned status Asyncify loses.
    return webcpp::test::exit_status(boost::report_errors());
}
