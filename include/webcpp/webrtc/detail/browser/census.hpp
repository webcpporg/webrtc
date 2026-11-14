// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_WEBRTC_DETAIL_BROWSER_CENSUS_HPP
#define WEBCPP_WEBRTC_DETAIL_BROWSER_CENSUS_HPP

#include <webcpp/webrtc/config.hpp>

#include <emscripten/em_asm.h>
#include <emscripten/val.h>

#include <cstddef>

// The page's census of the RTCPeerConnection objects webcpp::webrtc made (design.md, D6): it
// lives in JavaScript, on the page, under Symbol.for("webcpp.webrtc.census"), so that every
// runtime and every module of the page counts in one place, and C++ keeps no state of it. A
// connection counts from its construction until a FinalizationRegistry sees it collected, as
// Chromium counts its limit of connections a page; without FinalizationRegistry nothing is
// counted, so that the page never stops making connections for a count that never comes down, as
// trystero's census does.

// clang-format off
// The census: {held, add(connection)}, made on the page's first call.
inline emscripten::EM_VAL webcpp_webrtc_census() {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const key = Symbol.for('webcpp.webrtc.census');
    let census = globalThis[key];
    if (!census) {
        census = {held: 0};
        const registry = typeof FinalizationRegistry === 'function'
            ? new FinalizationRegistry(() => { --census.held; }) : null;
        census.add = connection => {
            if (registry) {
                ++census.held;
                registry.register(connection, 0);
            }
        };
        globalThis[key] = census;
    }
    return Emval.toHandle(census);
    })));
}
// The connections the census counts.
inline double webcpp_webrtc_census_held() {
    return EM_ASM_DOUBLE(({
    const census = globalThis[Symbol.for('webcpp.webrtc.census')];
    const held = census ? Number(census.held) : 0;
    return held > 0 ? held : 0;
    }));
}
// clang-format on

namespace webcpp::webrtc::detail::browser {

/**
 The page's census, which `make` counts each connection in.

 @return The census.
*/
inline emscripten::val census() {
    return emscripten::val::take_ownership(webcpp_webrtc_census());
}

/**
 The connections of the page the census counts: made by webcpp::webrtc, with any runtime, and not
 yet collected.

 @return Their number.
*/
inline std::size_t census_held() noexcept {
    return static_cast<std::size_t>(webcpp_webrtc_census_held());
}

}  // namespace webcpp::webrtc::detail::browser

#endif  // WEBCPP_WEBRTC_DETAIL_BROWSER_CENSUS_HPP
