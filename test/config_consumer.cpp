// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The other side of config_provider.cpp's boundary: with the same policy it links, and with the
// other (WEBCPP_TEST_WEBRTC_MISMATCH) the channel it passes is another type in the browser, and
// the program does not link (design.md, D14).
#undef WEBCPP_WEBRTC_UNSUPPORTED_IGNORE
#undef WEBCPP_WEBRTC_UNSUPPORTED_ERROR
#ifdef WEBCPP_TEST_WEBRTC_MISMATCH
#define WEBCPP_WEBRTC_UNSUPPORTED_IGNORE
#else
#define WEBCPP_WEBRTC_UNSUPPORTED_ERROR
#endif
#include <webcpp/webrtc/data_channel.hpp>

void configured_channel_boundary(webcpp::webrtc::data_channel* channel);

int main() {
    // Nothing suspends: main's own status is the test's.
    configured_channel_boundary(nullptr);
}
