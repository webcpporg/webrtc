# Working on webrtc

Read the superproject's [AGENTS.md](../../AGENTS.md) and
[CODING-GUIDELINES.md](../../CODING-GUIDELINES.md) first. The library's proposal, its design
and its plan are in `proposals/libraries/webrtc/openspec/changes/genesis`.

- **Natively, libdatachannel's C API only** (`rtc/rtc.h`), never its C++ API: the C API catches
  every exception libdatachannel throws and returns a negative code, which is what lets this
  library work with exceptions on and off (design.md, D4). A new call maps its codes to
  `webcpp.webrtc` as the browser reports the same call (design.md, D10; plan, Task 1.4).
- **libdatachannel's callbacks run on its threads.** Each copies what it carries into the
  standard allocator and posts to the executor through the object's weak pointer; none runs a
  handler, none touches the user's allocator (design.md, D7). Destruction runs on the executor
  and calls `rtcDelete*`, which waits for the callbacks in flight; never call it from a callback.
- **A channel's queue is libdatachannel's.** Past 1,024 unread messages its thread waits for
  room, and the connection's other channels stall (measured, `test/stall_test.cpp`). Closing or
  destroying a channel drains its queue; keep that, or a thread waits on (design.md, D9).
- **The runtime is the process's.** One at a time natively, made before any connection and
  destroyed after the last connection and channel; tests make one per case.
- **Tests use real peers over the loopback,** bound to 127.0.0.1, with no STUN, and every run of
  the io_context says why after `lint-run:`. Each suite runs as the build asks and, where it
  matters, with `<exception-handling>off`. The lifetime test runs under ThreadSanitizer with
  `thread-sanitizer=on define=WEBCPP_TEST_WEBRTC_REPEAT=100`.
- **In the browser** (`detail/browser/`) each connection and channel is one
  `intrusive_registration` of webcpp::asio's bridge, as webcpp::beast's browser WebSocket is
  (design.md, D8): every call into the page is an `EM_ASM` function of `detail/browser/imports.hpp`
  that catches JavaScript's exceptions in JavaScript and reports their name; `deliver` only
  records and posts one notification per batch; a send is checked before the browser sees it.
  Under Node, which has no `RTCPeerConnection`, tests use a fake of their own
  (`test/browser_backend_test.cpp`); real-browser behaviour is the `browser` lane's
  (`test/browser/`, headless Chrome).
- **The unsupported policy** (D14) names the inline namespace the classes live in, on both
  targets, so that translation units that disagree on it fail to link (`config_mismatch`).
  Natively it decides a channel's `max_packet_life_time` of 0, which libdatachannel's C API
  takes as 0 retransmissions; in the browser, `configuration::bind_address`.
- **Increment 3** moves trystero's WebRTC onto this library, a change of trystero's.
