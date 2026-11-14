// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The harness's own probe: a program built as the lane's programs are, in both suspension
// variants, which reads in the page whether it has RTCPeerConnection.

#include <emscripten/em_asm.h>

#include <cstdio>

int main() {
    const int has = EM_ASM_INT({ return typeof RTCPeerConnection == 'function' ? 1 : 0; });
    std::puts(has != 0 ? "browser harness: ok" : "check failed: no RTCPeerConnection");
}
