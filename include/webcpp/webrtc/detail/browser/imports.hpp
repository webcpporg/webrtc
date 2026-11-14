// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_WEBRTC_DETAIL_BROWSER_IMPORTS_HPP
#define WEBCPP_WEBRTC_DETAIL_BROWSER_IMPORTS_HPP

#include <webcpp/webrtc/config.hpp>

#include <webcpp/webrtc/error.hpp>

#include <emscripten/em_asm.h>
#include <emscripten/val.h>

#include <boost/system/error_code.hpp>

#include <cstddef>
#include <string_view>

// The browser's WebRTC, called from C++ (design.md, D8). Each function is an EM_ASM block in an
// inline function, which Emscripten allows in several translation units, as webcpp::beast's
// browser/detail/imports.hpp does. JavaScript stays in JavaScript: every exception is caught there,
// before it can reach a WebAssembly frame, and reported as its name, the DOMException's (an
// exception without a name as ''); a promise is followed in JavaScript, and its settlement is
// delivered later through the callback, never during the call, so that no import enters C++.
//
// What JavaScript delivers through a callback is an object whose `kind` says what it is, in the
// numbering of webcpp::webrtc::detail::browser::event_kind below. Each enumeration crosses as the
// index of its W3C string in the order of webcpp::webrtc's enumeration of the same name, -1 for a
// string it does not know.
//
// The functions use Emval and UTF8ToString, which webcpp::asio's browser runtime has Emscripten
// link (<webcpp/asio/browser/src.hpp>, in every program on /webcpp/asio//asio).

// clang-format off
// Makes an RTCPeerConnection of `configuration`, counts it in `census` and sets the handlers of
// its events, which `callback` delivers: {pc}, or {error: name}.
inline emscripten::EM_VAL webcpp_webrtc_make(emscripten::EM_VAL configuration, emscripten::EM_VAL callback, emscripten::EM_VAL census) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const configuration = $0;
    const callback = $1;
    const census = $2;

    const deliver = Emval.toValue(callback);
    try {
        const pc = new RTCPeerConnection(Emval.toValue(configuration));
        Emval.toValue(census).add(pc);
        const states = ['new', 'connecting', 'connected', 'disconnected', 'failed', 'closed'];
        const ice = ['new', 'checking', 'connected', 'completed', 'disconnected', 'failed', 'closed'];
        pc.onconnectionstatechange = () => deliver({kind: 1, state: states.indexOf(pc.connectionState)});
        pc.oniceconnectionstatechange = () => deliver({kind: 2, state: ice.indexOf(pc.iceConnectionState)});
        pc.onicecandidate = event => {
            const candidate = event.candidate;
            if (candidate === null || candidate === undefined) {
                deliver({kind: 3, end: true});
                return;
            }
            // An empty candidate ends one generation's candidates; the null event that follows
            // ends the gathering, which is delivered once, as natively.
            if (!candidate.candidate) return;
            deliver({kind: 3, end: false, candidate: candidate.candidate,
                sdpMid: candidate.sdpMid ?? null, sdpMLineIndex: candidate.sdpMLineIndex ?? null,
                usernameFragment: candidate.usernameFragment ?? null});
        };
        pc.ondatachannel = event => deliver({kind: 4, channel: event.channel});
        return Emval.toHandle({pc});
    } catch (error) {
        return Emval.toHandle({error: typeof error?.name === 'string' ? error.name : ""});
    }
    }), configuration, callback, census));
}
// Applies the implicit local description, `setLocalDescription()`; delivers {kind: 5, local:
// true, id, ok, type, sdp} once it settles, or {..., ok: false, name}.
inline void webcpp_webrtc_set_local(emscripten::EM_VAL pc, emscripten::EM_VAL callback, double id) {
    EM_ASM(({
    const pc = $0;
    const callback = $1;
    const id = $2;

    const connection = Emval.toValue(pc);
    const deliver = Emval.toValue(callback);
    const types = ['offer', 'answer', 'pranswer', 'rollback'];
    new Promise(resolve => resolve(connection.setLocalDescription())).then(() => {
        const description = connection.localDescription;
        return {kind: 5, local: true, id, ok: true, type: types.indexOf(description.type),
            sdp: description.sdp};
    }).then(event => event, error => ({kind: 5, local: true, id, ok: false,
        name: typeof error?.name === 'string' ? error.name : ""})).then(deliver);
    }), pc, callback, id);
}
// Applies a remote description of the type at index `type` and the SDP `sdp`; delivers
// {kind: 5, local: false, id, ok} once it settles, with the name when it is rejected.
inline void webcpp_webrtc_set_remote(emscripten::EM_VAL pc, int type, emscripten::EM_VAL sdp, emscripten::EM_VAL callback, double id) {
    EM_ASM(({
    const pc = $0;
    const type = $1;
    const sdp = $2;
    const callback = $3;
    const id = $4;

    const connection = Emval.toValue(pc);
    const description = {type: ['offer', 'answer', 'pranswer', 'rollback'][type], sdp: Emval.toValue(sdp)};
    const deliver = Emval.toValue(callback);
    new Promise(resolve => resolve(connection.setRemoteDescription(description))).then(
        () => ({kind: 5, local: false, id, ok: true}),
        error => ({kind: 5, local: false, id, ok: false,
            name: typeof error?.name === 'string' ? error.name : ""})).then(deliver);
    }), pc, type, sdp, callback, id);
}
// Adds the remote candidate `init`, an RTCIceCandidateInit; delivers {kind: 5, local: false, id,
// ok} once it settles, with the name when it is rejected.
inline void webcpp_webrtc_add_candidate(emscripten::EM_VAL pc, emscripten::EM_VAL init, emscripten::EM_VAL callback, double id) {
    EM_ASM(({
    const pc = $0;
    const init = $1;
    const callback = $2;
    const id = $3;

    const connection = Emval.toValue(pc);
    const candidate = Emval.toValue(init);
    const deliver = Emval.toValue(callback);
    new Promise(resolve => resolve(connection.addIceCandidate(candidate))).then(
        () => ({kind: 5, local: false, id, ok: true}),
        error => ({kind: 5, local: false, id, ok: false,
            name: typeof error?.name === 'string' ? error.name : ""})).then(deliver);
    }), pc, init, callback, id);
}
// Creates a data channel, `createDataChannel(label, init)`: {channel}, or {error: name}.
inline emscripten::EM_VAL webcpp_webrtc_channel(emscripten::EM_VAL pc, emscripten::EM_VAL label, emscripten::EM_VAL init) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const pc = $0;
    const label = $1;
    const init = $2;

    try {
        const channel = Emval.toValue(pc).createDataChannel(Emval.toValue(label), Emval.toValue(init));
        return Emval.toHandle({channel});
    } catch (error) {
        return Emval.toHandle({error: typeof error?.name === 'string' ? error.name : ""});
    }
    }), pc, label, init));
}
// Sets the handlers of a data channel's events, which `callback` delivers, its messages as
// ArrayBuffer objects or strings: null, or the name.
inline emscripten::EM_VAL webcpp_webrtc_attach(emscripten::EM_VAL channel, emscripten::EM_VAL callback) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const channel = $0;
    const callback = $1;

    const target = Emval.toValue(channel);
    const deliver = Emval.toValue(callback);
    try {
        target.binaryType = 'arraybuffer';
        target.onopen = () => deliver({kind: 6});
        target.onmessage = event => deliver({kind: 7, data: event.data,
            text: typeof event.data === 'string'});
        target.onclose = () => deliver({kind: 8});
        target.onbufferedamountlow = () => deliver({kind: 9});
        return Emval.toHandle(null);
    } catch (error) {
        return Emval.toHandle(typeof error?.name === 'string' ? error.name : "");
    }
    }), channel, callback));
}
// Sets to null the handlers a connection's (`channel` false) or a channel's (true) events were
// given, so that none is delivered from now on: null, or the name.
inline emscripten::EM_VAL webcpp_webrtc_detach(emscripten::EM_VAL object, bool channel) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const object = $0;
    const channel = $1;

    const target = Emval.toValue(object);
    const names = channel ? ['onopen', 'onmessage', 'onclose', 'onbufferedamountlow']
        : ['onconnectionstatechange', 'oniceconnectionstatechange', 'onicecandidate', 'ondatachannel'];
    let failure = null;
    for (const name of names) {
        try {
            target[name] = null;
        } catch (error) {
            if (failure === null) failure = typeof error?.name === 'string' ? error.name : "";
        }
    }
    return Emval.toHandle(failure);
    }), object, channel));
}
// Closes a connection or a channel, `close()`: null, or the name.
inline emscripten::EM_VAL webcpp_webrtc_close(emscripten::EM_VAL object) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const object = $0;

    try {
        Emval.toValue(object).close();
        return Emval.toHandle(null);
    } catch (error) {
        return Emval.toHandle(typeof error?.name === 'string' ? error.name : "");
    }
    }), object));
}
// Sends `size` bytes at `data`, as a text (UTF-8, which must be valid) or as binary: null, or the
// name.
inline emscripten::EM_VAL webcpp_webrtc_send(emscripten::EM_VAL channel, const void* data, std::size_t size, bool text) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const channel = $0;
    const data = $1 >>> 0;
    const size = $2 >>> 0;
    const text = $3;

    try {
        const view = HEAPU8.subarray(data, data + size);
        // send and TextDecoder require data that is not shared.
        const bytes = view.buffer instanceof ArrayBuffer ? view : view.slice();
        Emval.toValue(channel).send(text ? new TextDecoder('utf-8', {fatal: true}).decode(bytes)
            : bytes);
        return Emval.toHandle(null);
    } catch (error) {
        return Emval.toHandle(typeof error?.name === 'string' ? error.name : "");
    }
    }), channel, data, size, text));
}
// The error of webcpp::webrtc that a name is: InvalidStateError 1, OperationError 2,
// InvalidAccessError 3, TypeError 4, SyntaxError 5, any other 2.
inline int webcpp_webrtc_error_of(emscripten::EM_VAL name) {
    return EM_ASM_INT(({
    const name = $0;

    const known = ['InvalidStateError', 'OperationError', 'InvalidAccessError', 'TypeError', 'SyntaxError'];
    const index = known.indexOf(Emval.toValue(name));
    return index < 0 ? 2 : index + 1;
    }), name);
}
// A channel's readyState, as the index of data_channel_state; closed for one it cannot read.
inline int webcpp_webrtc_ready_state(emscripten::EM_VAL channel) {
    return EM_ASM_INT(({
    const channel = $0;

    try {
        const index = ['connecting', 'open', 'closing', 'closed'].indexOf(Emval.toValue(channel).readyState);
        return index < 0 ? 3 : index;
    } catch (_) {
        return 3;
    }
    }), channel);
}
// A channel's bufferedAmount; 0 for one it cannot read.
inline double webcpp_webrtc_buffered_amount(emscripten::EM_VAL channel) {
    return EM_ASM_DOUBLE(({
    const channel = $0;

    try {
        const amount = Number(Emval.toValue(channel).bufferedAmount);
        return Number.isFinite(amount) && amount > 0 ? amount : 0;
    } catch (_) {
        return 0;
    }
    }), channel);
}
// Sets a channel's bufferedAmountLowThreshold, an unsigned long: null, or the name.
inline emscripten::EM_VAL webcpp_webrtc_set_threshold(emscripten::EM_VAL channel, double threshold) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const channel = $0;
    const threshold = $1;

    try {
        Emval.toValue(channel).bufferedAmountLowThreshold = threshold;
        return Emval.toHandle(null);
    } catch (error) {
        return Emval.toHandle(typeof error?.name === 'string' ? error.name : "");
    }
    }), channel, threshold));
}
// A channel's id; -1 while it has none, or for one it cannot read.
inline int webcpp_webrtc_channel_id(emscripten::EM_VAL channel) {
    return EM_ASM_INT(({
    const channel = $0;

    try {
        const id = Emval.toValue(channel).id;
        return Number.isInteger(id) && id >= 0 && id <= 65535 ? id : -1;
    } catch (_) {
        return -1;
    }
    }), channel);
}
// A channel's label; '' for one it cannot read.
inline emscripten::EM_VAL webcpp_webrtc_label(emscripten::EM_VAL channel) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const channel = $0;

    try {
        return Emval.toHandle(String(Emval.toValue(channel).label));
    } catch (_) {
        return Emval.toHandle("");
    }
    }), channel));
}
// The largest message the connection's SCTP transport sends, maxMessageSize; 0 while it has none
// or for one it cannot read, and Infinity for no limit.
inline double webcpp_webrtc_peer_max_message_size(emscripten::EM_VAL pc) {
    return EM_ASM_DOUBLE(({
    const pc = $0;

    try {
        const sctp = Emval.toValue(pc).sctp;
        const size = sctp ? Number(sctp.maxMessageSize) : 0;
        return size > 0 ? size : 0;
    } catch (_) {
        return 0;
    }
    }), pc);
}
// Whether a connection has a remoteDescription; false for one it cannot read.
inline bool webcpp_webrtc_has_remote(emscripten::EM_VAL pc) {
    return EM_ASM_INT(({
    const pc = $0;

    try {
        const description = Emval.toValue(pc).remoteDescription;
        return description !== null && description !== undefined ? 1 : 0;
    } catch (_) {
        return 0;
    }
    }), pc) != 0;
}
// A connection's localDescription, {type, sdp} with the type as the index of sdp_type; null when
// it has none, or for one it cannot read.
inline emscripten::EM_VAL webcpp_webrtc_local_description(emscripten::EM_VAL pc) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const pc = $0;

    try {
        const description = Emval.toValue(pc).localDescription;
        if (description === null || description === undefined) return Emval.toHandle(null);
        return Emval.toHandle({type: ['offer', 'answer', 'pranswer', 'rollback'].indexOf(description.type),
            sdp: String(description.sdp)});
    } catch (_) {
        return Emval.toHandle(null);
    }
    }), pc));
}
// The size in bytes of a string, as UTF-8, or of an ArrayBuffer or a view of one.
inline double webcpp_webrtc_size(emscripten::EM_VAL value) {
    return EM_ASM_DOUBLE(({
    const value = $0;

    const data = Emval.toValue(value);
    if (typeof data === 'string') return new TextEncoder().encode(data).length;
    return data?.byteLength ?? 0;
    }), value);
}
// Copies a string, as UTF-8, or an ArrayBuffer or a view of one, into `size` bytes at `data`.
// NOLINTNEXTLINE(misc-const-correctness): JavaScript writes to the pointed WebAssembly storage
inline void webcpp_webrtc_copy(emscripten::EM_VAL value, void* data, std::size_t size) {
    EM_ASM(({
    const value = $0;
    const data = $1 >>> 0;
    const size = $2 >>> 0;

    const source = Emval.toValue(value);
    // The view is taken after C++ prepared the storage, which may have grown the memory.
    const destination = HEAPU8.subarray(data, data + size);
    if (typeof source === 'string') {
        const encoder = new TextEncoder();
        // encodeInto refuses a view of a SharedArrayBuffer.
        if (destination.buffer instanceof ArrayBuffer) encoder.encodeInto(source, destination);
        else destination.set(encoder.encode(source));
    } else if (ArrayBuffer.isView(source)) {
        destination.set(new Uint8Array(source.buffer, source.byteOffset, Math.min(size, source.byteLength)));
    } else if (source !== null && source !== undefined) {
        destination.set(new Uint8Array(source, 0, Math.min(size, source.byteLength)));
    }
    }), value, data, size);
}
// The JavaScript string of `size` bytes of UTF-8 at `data`.
inline emscripten::EM_VAL webcpp_webrtc_string(const char* data, std::size_t size) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const data = $0 >>> 0;
    const size = $1 >>> 0;

    return Emval.toHandle(UTF8ToString(data, size));
    }), data, size));
}
// clang-format on

namespace webcpp::webrtc::detail::browser {

/** The error type of every completion. */
using boost::system::error_code;

/** What an event a callback delivers is, its `kind`. */
enum class event_kind {
    /** A connection's `connectionstatechange`: `state`. */
    connection_state = 1,
    /** A connection's `iceconnectionstatechange`: `state`. */
    ice_state = 2,
    /** A connection's `icecandidate`: `end`, or the candidate's members. */
    candidate = 3,
    /** A connection's `datachannel`: `channel`. */
    channel = 4,
    /** A promise settled: `local`, `id`, `ok`, and `name`, or a local description's members. */
    settled = 5,
    /** A channel's `open`. */
    open = 6,
    /** A channel's `message`: `data` and `text`. */
    message = 7,
    /** A channel's `close`. */
    close = 8,
    /** A channel's `bufferedamountlow`. */
    buffered_amount_low = 9,
};

/**
 Takes ownership of a handle an import returned.

 @param handle The handle.
 @return The value, which frees the handle when it goes.
*/
inline emscripten::val take(emscripten::EM_VAL handle) {
    return emscripten::val::take_ownership(handle);
}

/**
 What a delivered event is.

 @param event The event.
 @return Its kind, or 0 for an object that is no event of these imports.
*/
inline int kind_of(const emscripten::val& event) {
    const emscripten::val kind = event["kind"];
    return kind.isNumber() ? kind.as<int>() : 0;
}

/**
 The error of webcpp::webrtc that an exception's or a rejection's name is (design.md, D10).

 @param name The name, a string.
 @return The code: `InvalidStateError`, `OperationError`, `InvalidAccessError`, `TypeError` and
 `SyntaxError` as the enumerator of the same name, any other name as `error::operation_error`.
*/
inline error_code error_of(const emscripten::val& name) {
    return make_error_code(static_cast<error>(webcpp_webrtc_error_of(name.as_handle())));
}

/**
 What an import that returns null or a name reported.

 @param outcome What it returned.
 @return Nothing for null, else the error of the name.
*/
inline error_code outcome_of(const emscripten::val& outcome) {
    if (outcome.isNull() || outcome.isUndefined()) {
        return {};
    }
    return error_of(outcome);
}

/**
 The JavaScript string of a UTF-8 text.

 @param text The text, read during the call.
 @return The string.
*/
inline emscripten::val js_string(std::string_view text) {
    return take(webcpp_webrtc_string(text.data(), text.size()));
}

/**
 Copies a JavaScript string, as UTF-8, or an `ArrayBuffer` into `out`, in its own allocator.

 @tparam Bytes A contiguous container of bytes, as a string or a vector.
 @param value The string or the buffer.
 @param out What it is copied into, resized to it.
*/
template <class Bytes>
void js_copy(const emscripten::val& value, Bytes& out) {
    out.resize(static_cast<std::size_t>(webcpp_webrtc_size(value.as_handle())));
    if (!out.empty()) {
        webcpp_webrtc_copy(value.as_handle(), out.data(), out.size());
    }
}

}  // namespace webcpp::webrtc::detail::browser

#endif  // WEBCPP_WEBRTC_DETAIL_BROWSER_IMPORTS_HPP
