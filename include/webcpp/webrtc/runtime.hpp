// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_WEBRTC_RUNTIME_HPP
#define WEBCPP_WEBRTC_RUNTIME_HPP

#include <webcpp/webrtc/config.hpp>

#include <webcpp/webrtc/options.hpp>

#ifdef WEBCPP_TARGET_EMSCRIPTEN
#include <webcpp/webrtc/detail/browser/census.hpp>
#else
#include <webcpp/webrtc/detail/native/capi.hpp>
#endif

#include <atomic>
#include <cstddef>
#include <utility>

#ifndef WEBCPP_TARGET_EMSCRIPTEN
namespace webcpp::webrtc::detail {
struct runtime_access;
}  // namespace webcpp::webrtc::detail
#endif

// In the browser the classes live in an inline namespace named after the unsupported policy, so
// that translation units that disagree on it name different types and fail to link
// (design.md, D14); natively, where every setting is honoured, they do not.
// NOLINTNEXTLINE(modernize-concat-nested-namespaces): the policy selects an inline namespace
namespace webcpp::webrtc {
#ifdef WEBCPP_TARGET_EMSCRIPTEN
#ifdef WEBCPP_WEBRTC_UNSUPPORTED_IGNORE
inline namespace unsupported_ignore {
#else
inline namespace unsupported_error {
#endif
#endif

/**
 What the platform holds for the whole process, or the whole page, which the program owns:
 natively libdatachannel's threads and global state, and on both targets the cap of peer
 connections.

 A program makes one before any peer connection and destroys it after the last, and after the
 last data channel; `make` takes it by reference, so no connection exists without one.

 Natively at most one exists at a time in a process, because libdatachannel's state is the
 process's: a second one's destructor would end the first one's connections. That is a
 precondition: webrtc keeps no static state to check it with. A native runtime counts the
 connections made with it.

 In the browser the count is the page's, kept in JavaScript, so that every runtime and every
 module of the page counts in one place, as Chromium counts its own limit: a connection counts
 from its construction until the garbage collector frees it, a closed one included.

 @see "The runtime", in the guide.
*/
class runtime {
public:
    /**
     Makes the runtime.

     Natively it loads libdatachannel's global resources (`rtcPreload`), with its thread pool sized
     as `opts` says; in the browser, whose threads are its own, `thread_pool_size` changes
     nothing.

     @param opts The cap of peer connections and, natively, the thread pool's size.
    */
    explicit runtime(const runtime_options& opts = {}) : options_(opts) {
#ifndef WEBCPP_TARGET_EMSCRIPTEN
        if (opts.thread_pool_size != 0) {
            rtcSetThreadPoolSize(static_cast<unsigned int>(opts.thread_pool_size));
        }
        rtcPreload();
#endif
    }

    // In the browser the destructor has nothing to do; natively it does, and one declaration
    // carries the Doc Comment of both.
    // NOLINTBEGIN(modernize-use-equals-default)
    /**
     Destroys the runtime.

     Natively it unloads libdatachannel's global resources (`rtcCleanup`), waiting until its
     threads have stopped, for at most ten seconds, after which libdatachannel gives up; in the
     browser it holds nothing of the page.
    */
    ~runtime() {
#ifndef WEBCPP_TARGET_EMSCRIPTEN
        rtcCleanup();
#endif
    }

    // NOLINTEND(modernize-use-equals-default)

    /** Not copyable: its connections hold it by reference, and natively it is the process's. */
    runtime(const runtime&) = delete;
    /**
     Not copy-assignable: its connections hold it by reference, and natively it is the
     process's.
    */
    runtime& operator=(const runtime&) = delete;
    /** Not movable: its connections hold it by reference. */
    runtime(runtime&&) = delete;
    /** Not move-assignable: its connections hold it by reference. */
    runtime& operator=(runtime&&) = delete;

    /**
     The options it was made with.

     @return The options.
    */
    [[nodiscard]] const runtime_options& get_options() const noexcept { return options_; }

    // A member on both targets: natively it reads the runtime's own count; in the browser the
    // page's, which lives in JavaScript.
    // NOLINTBEGIN(readability-convert-member-functions-to-static)
    /**
     The peer connections alive that count against the cap: natively those made with it; in the
     browser the page's, made by webcpp::webrtc with any runtime and not yet freed by the garbage
     collector.

     @return Their number.
    */
    [[nodiscard]] std::size_t peer_connections() const noexcept {
#ifdef WEBCPP_TARGET_EMSCRIPTEN
        return detail::browser::census_held();
#else
        return live_.load();
#endif
    }

    // NOLINTEND(readability-convert-member-functions-to-static)

private:
#ifndef WEBCPP_TARGET_EMSCRIPTEN
    friend struct detail::runtime_access;

    bool acquire() noexcept {
        std::size_t live = live_.load();
        do {
            if (live >= options_.max_peer_connections) {
                return false;
            }
        } while (!live_.compare_exchange_weak(live, live + 1));
        return true;
    }

    void release() noexcept { live_.fetch_sub(1); }
#endif

    runtime_options options_;
#ifndef WEBCPP_TARGET_EMSCRIPTEN
    // Connections may live on several executors and threads, so the count is atomic.
    std::atomic<std::size_t> live_{0};
#endif
};

#ifdef WEBCPP_TARGET_EMSCRIPTEN
}  // namespace unsupported_error or unsupported_ignore
#endif
}  // namespace webcpp::webrtc

#ifdef WEBCPP_TARGET_EMSCRIPTEN

// NOLINTNEXTLINE(modernize-concat-nested-namespaces): the policy selects an inline namespace
namespace webcpp::webrtc::detail {
#ifdef WEBCPP_WEBRTC_UNSUPPORTED_IGNORE
inline namespace unsupported_ignore {
#else
inline namespace unsupported_error {
#endif

/**
 One connection's place under a runtime's cap: held when the page's census counts fewer
 connections than the cap. The census itself counts the connection once it is made, and until it
 is collected, so the place gives nothing back.
*/
class runtime_slot {
public:
    /** No place. */
    runtime_slot() = default;

    /** Takes a place under the cap of `rt`, or none at it. */
    explicit runtime_slot(const runtime& rt) noexcept
        : held_(rt.peer_connections() < rt.get_options().max_peer_connections) {}

    /** Not copyable: it is one place. */
    runtime_slot(const runtime_slot&) = delete;
    /** Not copy-assignable: it is one place. */
    runtime_slot& operator=(const runtime_slot&) = delete;

    /** Moves the place. */
    runtime_slot(runtime_slot&& other) noexcept : held_(std::exchange(other.held_, false)) {}

    /** Moves the place. */
    runtime_slot& operator=(runtime_slot&& other) noexcept {
        held_ = std::exchange(other.held_, false);
        return *this;
    }

    /** Drops the place. */
    ~runtime_slot() = default;

    /** Whether it holds a place. */
    explicit operator bool() const noexcept { return held_; }

    /** Drops the place; the census gives it back once the connection is collected. */
    void reset() noexcept { held_ = false; }

private:
    bool held_ = false;
};

}  // namespace unsupported_error or unsupported_ignore
}  // namespace webcpp::webrtc::detail

#else

namespace webcpp::webrtc::detail {

/** The library's access to a runtime's count. */
struct runtime_access {
    /** Counts one more connection, unless the cap is reached. */
    static bool acquire(runtime& rt) noexcept { return rt.acquire(); }

    /** Counts one connection less. */
    static void release(runtime& rt) noexcept { rt.release(); }
};

/** One connection's place in a runtime's count, given back when it goes. */
class runtime_slot {
public:
    /** No place. */
    runtime_slot() = default;

    /** Takes a place in `rt`, or none past its cap. */
    explicit runtime_slot(runtime& rt) noexcept
        : runtime_(runtime_access::acquire(rt) ? &rt : nullptr) {}

    /** Not copyable: it is one place, given back once. */
    runtime_slot(const runtime_slot&) = delete;
    /** Not copy-assignable: it is one place, given back once. */
    runtime_slot& operator=(const runtime_slot&) = delete;

    /** Moves the place. */
    runtime_slot(runtime_slot&& other) noexcept
        : runtime_(std::exchange(other.runtime_, nullptr)) {}

    /** Moves the place, giving this one's back. */
    runtime_slot& operator=(runtime_slot&& other) noexcept {
        if (this != &other) {
            reset();
            runtime_ = std::exchange(other.runtime_, nullptr);
        }
        return *this;
    }

    /** Gives the place back. */
    ~runtime_slot() { reset(); }

    /** Whether it holds a place. */
    explicit operator bool() const noexcept { return runtime_ != nullptr; }

    /** Gives the place back. */
    void reset() noexcept {
        if (runtime_ != nullptr) {
            runtime_access::release(*std::exchange(runtime_, nullptr));
        }
    }

private:
    runtime* runtime_ = nullptr;
};

}  // namespace webcpp::webrtc::detail

#endif  // WEBCPP_TARGET_EMSCRIPTEN

#endif  // WEBCPP_WEBRTC_RUNTIME_HPP
