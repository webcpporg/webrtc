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

#ifdef WEBCPP_TARGET_EMSCRIPTEN

// In the browser the classes live in an inline namespace named after the unsupported policy, so
// that translation units that disagree on it name different types and fail to link
// (design.md, D14).
// NOLINTNEXTLINE(modernize-concat-nested-namespaces): the policy selects an inline namespace
namespace webcpp::webrtc {
#ifdef WEBCPP_WEBRTC_UNSUPPORTED_IGNORE
inline namespace unsupported_ignore {
#else
inline namespace unsupported_error {
#endif

/**
 What the page holds for the whole program, which the program owns: the cap of peer connections.

 A program makes one before any peer connection and destroys it after the last, and after the
 last data channel; `make` takes it by reference, so no connection exists without one. The page's
 count of connections lives in JavaScript, on the page, so that every runtime and every module of
 the page counts in one place, as Chromium counts its own limit: a connection counts from its
 construction until the garbage collector frees it, a closed one included.

 @see "The runtime", in the guide.
*/
class runtime {
public:
    /**
     Makes the runtime; the browser's threads are its own, so `thread_pool_size` changes nothing.

     @param opts The cap of peer connections.
    */
    explicit runtime(const runtime_options& opts = {}) noexcept : options_(opts) {}

    /** Destroys the runtime, which holds nothing of the page. */
    ~runtime() = default;

    /** Not copyable: its connections hold it by reference. */
    runtime(const runtime&) = delete;
    /** Not copy-assignable: its connections hold it by reference. */
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

    /**
     The peer connections of the page that count against the cap: made by webcpp::webrtc, with any
     runtime, and not yet freed by the garbage collector.

     @return Their number.
    */
    // A member, as natively, where it reads the runtime's own count; the page's lives in
    // JavaScript.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    [[nodiscard]] std::size_t peer_connections() const noexcept {
        return detail::browser::census_held();
    }

private:
    runtime_options options_;
};

}  // namespace unsupported_error or unsupported_ignore
}  // namespace webcpp::webrtc

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

namespace webcpp::webrtc {

namespace detail {
struct runtime_access;
}  // namespace detail

/**
 What the platform holds for the whole process, which the program owns: natively libdatachannel's
 threads and global state, and on both targets the cap of peer connections.

 A program makes one before any peer connection and destroys it after the last, and after the
 last data channel; `make` takes it by reference, so no connection exists without one. Natively at
 most one exists at a time in a process, because libdatachannel's state is the process's: a
 second one's destructor would end the first one's connections. That is a precondition: webrtc
 keeps no static state to check it with.

 @see "The runtime", in the guide.
*/
class runtime {
public:
    /**
     Loads libdatachannel's global resources, with its thread pool sized as `opts` says.

     @param opts The cap of peer connections and the thread pool's size.
    */
    explicit runtime(const runtime_options& opts = {}) : options_(opts) {
        if (opts.thread_pool_size != 0) {
            rtcSetThreadPoolSize(static_cast<unsigned int>(opts.thread_pool_size));
        }
        rtcPreload();
    }

    /** Unloads libdatachannel's global resources, waiting until its threads have stopped. */
    ~runtime() { rtcCleanup(); }

    /** Not copyable: it is the process's state, held once. */
    runtime(const runtime&) = delete;
    /** Not copy-assignable: it is the process's state, held once. */
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

    /**
     The peer connections made with it that are alive.

     @return Their number.
    */
    [[nodiscard]] std::size_t peer_connections() const noexcept { return live_.load(); }

private:
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

    runtime_options options_;
    // Connections may live on several executors and threads, so the count is atomic.
    std::atomic<std::size_t> live_{0};
};

namespace detail {

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

}  // namespace detail

}  // namespace webcpp::webrtc

#endif  // WEBCPP_TARGET_EMSCRIPTEN

#endif  // WEBCPP_WEBRTC_RUNTIME_HPP
