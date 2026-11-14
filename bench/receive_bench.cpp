// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The C API's cost on receipt (tasks.md 1.11, design.md D4): what the native backend pays to read
// one message out of libdatachannel's queue through rtcReceiveMessage, measured against what
// libdatachannel's C++ API pays through receive(), the one other way to read it.
//
// Two peers on the loopback, made with each API, exchange binary messages of 1 KiB and of 64 KiB.
// Each round lets the sender fill the receiver's queue with up to 1,000 messages, below
// libdatachannel's limit of 1,024, and waits until all of them are queued; then it times draining
// the queue into one dynamic buffer and counts the allocations of the thread that drains it, which
// is the io_context's thread in a program. The webcpp::webrtc path is the backend's own function,
// detail::native::read_message; the C++ API's path is receive() and a copy into the same buffer,
// the copy any program reading into its own buffer makes. Every message is checked once its
// round is drained, before it counts.
//
// The C++ API throws, so this program needs exceptions; the library does not (design.md, D4).
// Built with WEBCPP_TEST_WEBRTC_BENCH_SMOKE it runs one or two rounds of each, as test/Jamfile's
// bench. A fault ends the program with its reason on standard error and the exit status 1.

#include <webcpp/webrtc.hpp>
#include <webcpp/webrtc/detail/native/channel.hpp>

#include <rtc/rtc.h>
#include <rtc/datachannel.hpp>
#include <rtc/peerconnection.hpp>

#include <webcpp/asio/buffer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>  // lint-world: the measurement's clock
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>  // lint-world: waits for libdatachannel's threads to fill a queue
#include <variant>
#include <vector>

namespace {

// This thread's allocations: the drain runs on the main thread, and libdatachannel's threads
// allocate beside it, each in its own count. A replaced operator new has no object to keep it in.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local std::size_t allocations = 0;

}  // namespace

// The standard library's declarations name the size as each implementation pleases.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size) {
    ++allocations;
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t /*size*/) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t /*size*/) noexcept {
    std::free(p);
}

namespace {

using clock = std::chrono::steady_clock;

/** The largest round: below libdatachannel's queue of 1,024 messages, so that none waits. */
constexpr std::size_t round_limit = 1000;

/** How long a connection or a round may take before the run fails. */
constexpr std::chrono::seconds patience{60};

/**
 One run of one path: the messages drained, the time draining took, the allocations it made, and
 the time the rounds took to arrive, from the first send to a full queue.
*/
struct measure {
    std::size_t messages = 0;
    std::size_t bytes = 0;
    clock::duration elapsed{};
    std::size_t allocations = 0;
    clock::duration arrival{};
};

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("receive_bench: " + what);
}

/** Waits until `ready()` holds, or fails naming `what`. */
template <class Predicate>
void await(Predicate ready, const char* what) {
    const auto limit = clock::now() + patience;
    while (!ready()) {
        if (clock::now() > limit) {
            fail(std::string("timed out waiting for ") + what);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/** The byte at `offset` of message `index`, so that a message read twice or out of order shows. */
char pattern(std::size_t index, std::size_t offset) {
    return static_cast<char>(((index * 131) + offset) % 251);
}

void fill(std::vector<char>& message, std::size_t index) {
    for (std::size_t offset = 0; offset < message.size(); ++offset) {
        message[offset] = pattern(index, offset);
    }
}

/** Checks that `storage` holds messages `first` to `first + count - 1`, each of `size` bytes. */
void verify(const std::vector<char>& storage, std::size_t first, std::size_t count,
            std::size_t size) {
    if (storage.size() != count * size) {
        fail("a round drained " + std::to_string(storage.size()) + " bytes, not " +
             std::to_string(count * size));
    }
    for (std::size_t m = 0; m < count; ++m) {
        for (std::size_t offset = 0; offset < size; ++offset) {
            if (storage[(m * size) + offset] != pattern(first + m, offset)) {
                fail("message " + std::to_string(first + m) + " differs at byte " +
                     std::to_string(offset));
            }
        }
    }
}

/** The answering peer and its end of the channel, as libdatachannel's C API calls back. */
struct c_peer {
    int pc = -1;
    std::atomic<int> dc{-1};
    c_peer* other = nullptr;
};

void RTC_API c_description(int /*pc*/, const char* sdp, const char* type, void* user) {
    rtcSetRemoteDescription(static_cast<c_peer*>(user)->other->pc, sdp, type);
}

void RTC_API c_candidate(int /*pc*/, const char* candidate, const char* mid, void* user) {
    rtcAddRemoteCandidate(static_cast<c_peer*>(user)->other->pc, candidate, mid);
}

void RTC_API c_channel(int /*pc*/, int dc, void* user) {
    static_cast<c_peer*>(user)->dc = dc;
}

void open_c_peer(c_peer& peer) {
    rtcConfiguration config{};
    config.bindAddress = "127.0.0.1";
    peer.pc = rtcCreatePeerConnection(&config);
    if (peer.pc < 0) {
        fail("rtcCreatePeerConnection failed with " + std::to_string(peer.pc));
    }
    rtcSetUserPointer(peer.pc, &peer);
    rtcSetLocalDescriptionCallback(peer.pc, c_description);
    rtcSetLocalCandidateCallback(peer.pc, c_candidate);
    rtcSetDataChannelCallback(peer.pc, c_channel);
}

/**
 Receives `total` messages of `size` bytes through the backend's read_message: two peers on the
 C API, the second reading in libdatachannel's pull mode, as the backend does.
*/
measure through_c_api(std::size_t total, std::size_t size) {
    c_peer offering;
    c_peer answering;
    offering.other = &answering;
    answering.other = &offering;
    open_c_peer(offering);
    open_c_peer(answering);
    const int tx = rtcCreateDataChannel(offering.pc, "bench");
    if (tx < 0) {
        fail("rtcCreateDataChannel failed with " + std::to_string(tx));
    }
    await([&] { return rtcIsOpen(tx) && answering.dc >= 0 && rtcIsOpen(answering.dc); },
          "the C API's channel to open");
    const int rx = answering.dc;

    measure result;
    std::vector<char> message(size);
    std::vector<char> storage;
    storage.reserve(std::min(total, round_limit) * size);
    const std::allocator<std::byte> allocator;
    for (std::size_t first = 0; first < total; first += round_limit) {
        const std::size_t count = std::min(round_limit, total - first);
        const auto sent = clock::now();
        for (std::size_t m = 0; m < count; ++m) {
            fill(message, first + m);
            if (rtcSendMessage(tx, message.data(), static_cast<int>(size)) < 0) {
                fail("rtcSendMessage failed");
            }
        }
        const auto expected = static_cast<int>(count * size);
        await([&] { return rtcGetAvailableAmount(rx) == expected; }, "a C API round to arrive");
        result.arrival += clock::now() - sent;

        storage.clear();
        auto buffer = webcpp::asio::dynamic_buffer(storage);
        const std::size_t before = allocations;
        const auto start = clock::now();
        for (std::size_t m = 0; m < count; ++m) {
            const auto read = webcpp::webrtc::detail::native::read_message<decltype(buffer)>(
                &buffer, rx, allocator);
            if (read.status != webcpp::webrtc::detail::native::read_status::message ||
                read.size != size || read.text) {
                fail("read_message did not read one binary message of the round");
            }
        }
        result.elapsed += clock::now() - start;
        result.allocations += allocations - before;
        verify(storage, first, count, size);
        result.messages += count;
        result.bytes += count * size;
    }
    rtcDeletePeerConnection(offering.pc);
    rtcDeletePeerConnection(answering.pc);
    return result;
}

/**
 Receives `total` messages of `size` bytes through the C++ API's receive(), copying each into the
 same kind of dynamic buffer: two peers on the C++ API, the second in libdatachannel's pull mode.
*/
measure through_cpp_api(std::size_t total, std::size_t size) {
    rtc::Configuration config;
    config.bindAddress = "127.0.0.1";
    rtc::PeerConnection offering(config);
    rtc::PeerConnection answering(config);
    offering.onLocalDescription(
        [&answering](const rtc::Description& d) { answering.setRemoteDescription(d); });
    offering.onLocalCandidate(
        [&answering](const rtc::Candidate& c) { answering.addRemoteCandidate(c); });
    answering.onLocalDescription(
        [&offering](const rtc::Description& d) { offering.setRemoteDescription(d); });
    answering.onLocalCandidate(
        [&offering](const rtc::Candidate& c) { offering.addRemoteCandidate(c); });
    std::mutex mutex;
    std::shared_ptr<rtc::DataChannel> incoming;
    answering.onDataChannel([&](const std::shared_ptr<rtc::DataChannel>& dc) {
        const std::scoped_lock lock(mutex);
        incoming = dc;
    });
    const auto tx = offering.createDataChannel("bench");
    std::shared_ptr<rtc::DataChannel> rx;
    await(
        [&] {
            const std::scoped_lock lock(mutex);
            rx = incoming;
            return tx->isOpen() && rx && rx->isOpen();
        },
        "the C++ API's channel to open");

    measure result;
    std::vector<std::byte> message(size);
    std::vector<char> pattern_bytes(size);
    std::vector<char> storage;
    storage.reserve(std::min(total, round_limit) * size);
    for (std::size_t first = 0; first < total; first += round_limit) {
        const std::size_t count = std::min(round_limit, total - first);
        const auto sent = clock::now();
        for (std::size_t m = 0; m < count; ++m) {
            fill(pattern_bytes, first + m);
            std::memcpy(message.data(), pattern_bytes.data(), size);
            tx->send(message);
        }
        await([&] { return rx->availableAmount() == count * size; }, "a C++ API round to arrive");
        result.arrival += clock::now() - sent;

        storage.clear();
        auto buffer = webcpp::asio::dynamic_buffer(storage);
        const std::size_t before = allocations;
        const auto start = clock::now();
        for (std::size_t m = 0; m < count; ++m) {
            const std::optional<rtc::message_variant> received = rx->receive();
            const auto* bytes = received ? std::get_if<rtc::binary>(&*received) : nullptr;
            if (bytes == nullptr || bytes->size() != size) {
                fail("receive() did not return one binary message of the round");
            }
            const auto region = buffer.prepare(size);
            std::memcpy(region.data(), bytes->data(), size);
            buffer.commit(size);
        }
        result.elapsed += clock::now() - start;
        result.allocations += allocations - before;
        verify(storage, first, count, size);
        result.messages += count;
        result.bytes += count * size;
    }
    tx->close();
    offering.close();
    answering.close();
    return result;
}

void report(const char* path, const measure& m) {
    const double seconds = std::chrono::duration<double>(m.elapsed).count();
    const double arrival = std::chrono::duration<double>(m.arrival).count();
    std::printf("  %-44s %12.0f %10.1f %12.2f %11.1f\n", path,
                static_cast<double>(m.messages) / seconds,
                static_cast<double>(m.bytes) / seconds / 1e6,
                static_cast<double>(m.allocations) / static_cast<double>(m.messages),
                static_cast<double>(m.bytes) / arrival / 1e6);
}

void compare(std::size_t total, std::size_t size) {
    const measure c_api = through_c_api(total, size);
    const measure cpp_api = through_cpp_api(total, size);
    std::printf("%zu binary messages of %zu bytes, drained from a full queue\n", total, size);
    std::printf("  %-44s %12s %10s %12s %11s\n", "path (draining)", "messages/s", "MB/s",
                "allocs/msg", "arrival MB/s");
    report("webcpp::webrtc, C API (rtcReceiveMessage)", c_api);
    report("libdatachannel C++ API (receive and a copy)", cpp_api);
    std::printf("  time per message, C API over C++ API: %.2f\n\n",
                std::chrono::duration<double>(c_api.elapsed).count() /
                    std::chrono::duration<double>(cpp_api.elapsed).count());
}

}  // namespace

// A fault is reported and fails the run, rather than ending it in std::terminate, whose abort()
// can stop a Windows run on a dialog.
int main() try {
    // libdatachannel's process-wide state, as a program holds it (design.md, D6).
    const webcpp::webrtc::runtime rt;
#ifdef WEBCPP_TEST_WEBRTC_BENCH_SMOKE
    compare(2000, 1024);
    compare(200, 65536);
#else
    compare(100000, 1024);
    compare(100000, 65536);
#endif
    return 0;
} catch (const std::exception& e) {
    static_cast<void>(std::fprintf(stderr, "%s\n", e.what()));
    return 1;
} catch (...) {
    static_cast<void>(
        std::fprintf(stderr, "receive_bench: an exception that is no std::exception\n"));
    return 1;
}
