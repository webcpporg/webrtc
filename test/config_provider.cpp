// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A translation unit built apart, with the policy that refuses what a target cannot honour,
// whichever the build asks for: it takes a channel across its boundary (design.md, D14).
#undef WEBCPP_WEBRTC_UNSUPPORTED_IGNORE
#undef WEBCPP_WEBRTC_UNSUPPORTED_ERROR
#define WEBCPP_WEBRTC_UNSUPPORTED_ERROR
#include <webcpp/webrtc/data_channel.hpp>

// NOLINTNEXTLINE(misc-use-internal-linkage): the external symbol is the policy's boundary
void configured_channel_boundary(webcpp::webrtc::data_channel* /*channel*/) {}
