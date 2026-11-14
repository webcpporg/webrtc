// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_WEBRTC_DETAIL_BROWSER_CHANNEL_HPP
#define WEBCPP_WEBRTC_DETAIL_BROWSER_CHANNEL_HPP

#include <webcpp/webrtc/config.hpp>

#include <webcpp/webrtc/detail/browser/imports.hpp>
#include <webcpp/webrtc/detail/operation.hpp>
#include <webcpp/webrtc/error.hpp>
#include <webcpp/webrtc/options.hpp>
#include <webcpp/webrtc/types.hpp>

#include <webcpp/asio/bind_allocator.hpp>  // lint-world: a notification in the object's allocator
#include <webcpp/asio/browser/bridge.hpp>  // lint-world: the browser's events
#include <webcpp/asio/buffer.hpp>          // lint-world: copies a message into the caller's buffer
#include <webcpp/asio/error.hpp>           // lint-world: Asio's codes
#include <webcpp/asio/execution/context.hpp>  // lint-world: the bridge of the executor's context
#include <webcpp/asio/post.hpp>               // lint-world: a notification is posted
#include <webcpp/asio/query.hpp>              // lint-world: the bridge of the executor's context

#include <emscripten/val.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// NOLINTNEXTLINE(modernize-concat-nested-namespaces): the policy selects an inline namespace
namespace webcpp::webrtc::detail::browser {
#ifdef WEBCPP_WEBRTC_UNSUPPORTED_IGNORE
inline namespace unsupported_ignore {
#else
inline namespace unsupported_error {
#endif

/**
 The bridge of the io_context an executor runs on, which delivers the browser's events to it.

 @param executor The object's executor.
 @return The bridge, a service of the executor's io_context.
*/
template <class Executor>
webcpp::asio::browser::bridge& bridge_of(const Executor& executor) {
    return webcpp::asio::use_service<webcpp::asio::browser::bridge>(
        webcpp::asio::query(executor, webcpp::asio::execution::context));
}

/**
 The state of a data channel in the browser: its `RTCDataChannel`, its received messages, and
 what waits on it, on the object's executor.

 It is one registration of the io_context's bridge, which delivers every event of the channel to
 `deliver`: an arrival is recorded there, a message copied into the queue and charged to
 `options::max_received_bytes` at once, and one notification posted for every arrival not yet
 drained, which completes the waits on the executor (design.md, D8 and D9).
*/
template <class Executor, class Allocator>
// The bridge indexes the registration, and the facade and the posted notifications share the
// state: two lifetimes of their own.
// NOLINTNEXTLINE(misc-multiple-inheritance)
class channel_state final
    : public webcpp::asio::browser::intrusive_registration,
      public std::enable_shared_from_this<channel_state<Executor, Allocator>> {
    using bytes_allocator = std::allocator_traits<Allocator>::template rebind_alloc<char>;

    /** A message received and not yet read, in the object's allocator. */
    struct message {
        std::vector<char, bytes_allocator> bytes;
        bool text = false;
    };

    using messages_allocator = std::allocator_traits<Allocator>::template rebind_alloc<message>;

public:
    /** The channel's text type. */
    using string_type = detail::string<Allocator>;

    /**
     What a message costs the receive budget beyond its payload: its record and the two links of
     its node, as webcpp::beast's `message_record_bytes` counts them.
    */
    static constexpr std::size_t message_record_bytes = sizeof(message) + (2 * sizeof(void*));

    /** Makes the state of a channel of the connection `connection`, not yet attached. */
    channel_state(Executor executor, const Allocator& allocator, const options& opts,
                  emscripten::val connection)
        : executor_(std::move(executor)),
          allocator_(allocator),
          label_(typename string_type::allocator_type(allocator)),
          connection_(std::move(connection)),
          messages_(messages_allocator(allocator)),
          max_message_size_(opts.max_message_size),
          max_buffered_amount_(opts.max_buffered_amount),
          max_received_bytes_(opts.max_received_bytes) {}

    /** Not copyable: the bridge delivers to it by its address. */
    channel_state(const channel_state&) = delete;
    /** Not copy-assignable: the bridge delivers to it by its address. */
    channel_state& operator=(const channel_state&) = delete;
    /** Not movable: the bridge delivers to it by its address. */
    channel_state(channel_state&&) = delete;
    /** Not move-assignable: the bridge delivers to it by its address. */
    channel_state& operator=(channel_state&&) = delete;

    /** Closes the channel, aborting what waits, and leaves the bridge. */
    // Posting the aborted completions allocates, and a failure to allocate there ends the
    // program, as in any noexcept destructor that must complete operations.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~channel_state() { destroy(); }

    /**
     Takes over the browser's channel: registers with the bridge, sets the handlers of its events,
     and reads its label and the state it is in already, since it may have opened, or closed.
    */
    void attach(emscripten::val channel) {
        channel_ = std::move(channel);
        bridge_ = &bridge_of(executor_);
        key_ = bridge_->add(*this);
        const emscripten::val callback = bridge_->function(key_);
        // A channel whose handlers cannot be set delivers nothing: it is closed.
        if (outcome_of(take(webcpp_webrtc_attach(channel_.as_handle(), callback.as_handle())))) {
            static_cast<void>(take(webcpp_webrtc_close(channel_.as_handle())));
            ready_ = data_channel_state::closed;
            accepting_ = false;
        } else {
            ready_ = live_state();
        }
        js_copy(take(webcpp_webrtc_label(channel_.as_handle())), label_);
    }

    /** The object's executor. */
    [[nodiscard]] const Executor& get_executor() const noexcept { return executor_; }

    /** The object's allocator. */
    [[nodiscard]] const Allocator& get_allocator() const noexcept { return allocator_; }

    /** Waits for the channel to open; at once when it is. */
    void wait_open(pending<>& op) {
        if (open_ != nullptr) {
            op.fail(webcpp::asio::error::already_started);
            return;
        }
        if (ready_ == data_channel_state::open) {
            op.complete({});
            return;
        }
        if (ready_ != data_channel_state::connecting) {
            op.fail(make_error_code(error::closed));
            return;
        }
        op.link(open_);
    }

    /** Waits for the channel to close; at once when it is. */
    void wait_close(pending<>& op) {
        if (close_ != nullptr) {
            op.fail(webcpp::asio::error::already_started);
            return;
        }
        if (ready_ == data_channel_state::closed) {
            op.complete({});
            return;
        }
        op.link(close_);
    }

    /** Waits for the buffered amount to fall to the threshold; at once when it is there. */
    void wait_buffered_amount_low(pending<>& op) {
        if (low_ != nullptr) {
            op.fail(webcpp::asio::error::already_started);
            return;
        }
        if (ready_ == data_channel_state::closing || ready_ == data_channel_state::closed) {
            op.fail(make_error_code(error::closed));
            return;
        }
        if (buffered_amount() <= threshold_) {
            op.complete({});
            return;
        }
        op.link(low_);
    }

    /** Waits for the next message, appended to `buffer`, which outlives the wait. */
    template <class DynamicBuffer>
    void receive(DynamicBuffer& buffer, pending<std::size_t, bool>& op) {
        if (receive_ != nullptr) {
            op.fail(webcpp::asio::error::already_started);
            return;
        }
        if (failure_) {
            op.fail(failure_);
            return;
        }
        if (!channel_.isUndefined() && ready_ == data_channel_state::closing) {
            op.fail(make_error_code(error::closed));
            return;
        }
        target_ = &buffer;
        append_ = &append<DynamicBuffer>;
        op.link(receive_);
        attempt();
        if (receive_ != nullptr &&
            (channel_.isUndefined() || ready_ == data_channel_state::closed)) {
            receive_->fail(make_error_code(error::closed));
        }
    }

    /**
     Sends one message, binary or text, at once or not at all: the channel's state, the size and
     the buffered amount are checked first, so that a refusal leaves the channel open
     (design.md, D9).

     @return The failure, or nothing once the browser queued it.
    */
    error_code send(const char* data, std::size_t size, bool text) {
        if (channel_.isUndefined() || live_state() != data_channel_state::open) {
            return make_error_code(error::invalid_state);
        }
        // The peer's limit is its SCTP transport's maxMessageSize, Infinity for none.
        const double peer = webcpp_webrtc_peer_max_message_size(connection_.as_handle());
        const bool within_peer = peer <= 0 || static_cast<double>(size) <= peer;
        if (size > max_message_size_ || !within_peer) {
            return make_error_code(error::message_too_large);
        }
        const std::size_t queued = buffered_amount();
        if (size > max_buffered_amount_ || queued > max_buffered_amount_ - size) {
            return make_error_code(error::buffered_amount_exceeded);
        }
        return outcome_of(take(webcpp_webrtc_send(channel_.as_handle(), data, size, text)));
    }

    /** Sets the threshold of `async_wait_buffered_amount_low`. */
    void set_buffered_amount_low_threshold(std::size_t threshold) {
        threshold_ = threshold;
        if (!channel_.isUndefined()) {
            // bufferedAmountLowThreshold is an unsigned long, which WebIDL would take modulo 2^32.
            const auto limit = static_cast<std::size_t>(0xFFFFFFFFU);
            static_cast<void>(take(webcpp_webrtc_set_threshold(
                channel_.as_handle(), static_cast<double>(std::min(threshold, limit)))));
        }
        buffered_amount_low();
    }

    /** The bytes queued to send, the browser's `bufferedAmount`. */
    [[nodiscard]] std::size_t buffered_amount() const {
        if (channel_.isUndefined()) {
            return 0;
        }
        return static_cast<std::size_t>(webcpp_webrtc_buffered_amount(channel_.as_handle()));
    }

    /** The channel's state. */
    [[nodiscard]] data_channel_state ready_state() const noexcept { return ready_; }

    /** The channel's label. */
    [[nodiscard]] std::string_view label() const noexcept { return label_; }

    /** The channel's SCTP stream id, once it has one. */
    [[nodiscard]] std::optional<std::uint16_t> id() const {
        const int stream =
            channel_.isUndefined() ? -1 : webcpp_webrtc_channel_id(channel_.as_handle());
        if (stream < 0) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(stream);
    }

    /**
     Closes the channel by its owner: what waits ends with `operation_aborted`, but
     `async_wait_close`, which completes once the browser closed it, and unread messages are
     discarded, as the W3C discards a message received once the channel left "open".
    */
    void close() {
        if (channel_.isUndefined() || ready_ == data_channel_state::closing ||
            ready_ == data_channel_state::closed) {
            return;
        }
        ready_ = data_channel_state::closing;
        abort(webcpp::asio::error::operation_aborted);
        discard_messages();
        static_cast<void>(take(webcpp_webrtc_close(channel_.as_handle())));
    }

    /**
     Closes the channel because its connection closed, which closes the browser's channel without
     an event: what waits ends with `error::closed`, and `async_wait_close` completes.
    */
    void connection_closed() {
        if (channel_.isUndefined() || ready_ == data_channel_state::closed) {
            return;
        }
        ready_ = data_channel_state::closed;
        abort(make_error_code(error::closed));
        discard_messages();
        if (close_ != nullptr) {
            close_->complete({});
        }
    }

    /**
     Closes the channel and leaves the bridge: what waits ends with `operation_aborted`, the
     handlers are set to null, and nothing the browser delivers reaches the state afterwards.
    */
    void destroy() {
        if (channel_.isUndefined()) {
            return;
        }
        ready_ = data_channel_state::closed;
        abort(webcpp::asio::error::operation_aborted);
        if (close_ != nullptr) {
            close_->fail(webcpp::asio::error::operation_aborted);
        }
        discard_messages();
        detach();
        static_cast<void>(take(webcpp_webrtc_close(channel_.as_handle())));
        channel_ = emscripten::val::undefined();
        connection_ = emscripten::val::undefined();
    }

    /**
     Records what the browser delivered and posts one notification for the arrivals not yet
     drained; it runs no handler and touches no caller's buffer.

     @param event The event, as the imports deliver it.
     @return False: the registration stays until the channel goes.
    */
    bool deliver(const emscripten::val& event) override {
        switch (static_cast<event_kind>(kind_of(event))) {
            case event_kind::open:
                if (ready_ == data_channel_state::connecting) {
                    ready_ = data_channel_state::open;
                }
                break;
            case event_kind::message: arrive(event); break;
            case event_kind::close: ready_ = data_channel_state::closed; break;
            case event_kind::buffered_amount_low: low_arrived_ = true; break;
            default: return false;
        }
        notify();
        return false;
    }

    /**
     The io_context's shutdown: what waits is destroyed without its handler being called, as
     Asio's own operations are, and the browser's channel is closed.
    */
    void shutdown() noexcept override {
        // A handler destroyed here may hold the last owner of the channel: the state lives until
        // this returns, and the handlers are destroyed after every change of it, as Asio's own
        // services destroy theirs.
        const auto keep = this->weak_from_this().lock();
        bridge_ = nullptr;
        key_ = 0;
        ++generation_;
        posted_ = false;
        accepting_ = false;
        pending<>* const open = std::exchange(open_, nullptr);
        pending<>* const close = std::exchange(close_, nullptr);
        pending<>* const low = std::exchange(low_, nullptr);
        pending<std::size_t, bool>* const receive = std::exchange(receive_, nullptr);
        messages_.clear();
        queued_ = 0;
        ready_ = data_channel_state::closed;
        if (!channel_.isUndefined()) {
            static_cast<void>(take(webcpp_webrtc_detach(channel_.as_handle(), true)));
            static_cast<void>(take(webcpp_webrtc_close(channel_.as_handle())));
            // A destruction that a handler starts below finds nothing left to do.
            channel_ = emscripten::val::undefined();
            connection_ = emscripten::val::undefined();
        }
        for (pending<>* const op : {open, close, low}) {
            if (op != nullptr) {
                op->discard();
            }
        }
        if (receive != nullptr) {
            receive->discard();
        }
    }

private:
    // Appends `m` to the dynamic buffer `opaque` points to; a message larger than the room left
    // in it is not read, and stays queued.
    template <class DynamicBuffer>
    static error_code append(void* opaque, const message& m) {
        auto& buffer = *static_cast<DynamicBuffer*>(opaque);
        if (m.bytes.size() > buffer.max_size() - buffer.size()) {
            return webcpp::asio::error::no_buffer_space;
        }
        buffer.commit(webcpp::asio::buffer_copy(
            buffer.prepare(m.bytes.size()), webcpp::asio::buffer(m.bytes.data(), m.bytes.size())));
        return {};
    }

    [[nodiscard]] data_channel_state live_state() const {
        return static_cast<data_channel_state>(webcpp_webrtc_ready_state(channel_.as_handle()));
    }

    // A message: past max_message_size, or past the receive budget with its record, the channel
    // closes, at once, so that the page's memory stops growing whatever the executor does
    // (design.md, D9); the failure reaches the receives on the executor.
    void arrive(const emscripten::val& event) {
        if (!accepting_ || failure_) {
            return;
        }
        const emscripten::val data = event["data"];
        const auto size = static_cast<std::size_t>(webcpp_webrtc_size(data.as_handle()));
        const std::size_t available = max_received_bytes_ - std::min(max_received_bytes_, queued_);
        if (size > max_message_size_ || size > available ||
            message_record_bytes > available - size) {
            failure_ = make_error_code(size > max_message_size_ ? error::message_too_large
                                                                : error::receive_overflow);
            accepting_ = false;
            discard_messages();
            if (ready_ == data_channel_state::connecting || ready_ == data_channel_state::open) {
                ready_ = data_channel_state::closing;
            }
            static_cast<void>(take(webcpp_webrtc_close(channel_.as_handle())));
            return;
        }
        message& m = messages_.emplace_back(message{
            std::vector<char, bytes_allocator>(bytes_allocator(allocator_)),
            event["text"].as<bool>(),
        });
        js_copy(data, m.bytes);
        queued_ += size + message_record_bytes;
    }

    // Posts one notification for the arrivals not yet drained, which a later state finds
    // nothing in.
    void notify() {
        if (posted_) {
            return;
        }
        posted_ = true;
        webcpp::asio::post(
            executor_, webcpp::asio::bind_allocator(
                           allocator_, [self = this->weak_from_this(), generation = generation_] {
                               const auto state = self.lock();
                               if (state && state->generation_ == generation) {
                                   state->drain();
                               }
                           }));
    }

    // Completes, on the executor, what the arrivals satisfy.
    void drain() {
        posted_ = false;
        if (ready_ == data_channel_state::open && open_ != nullptr) {
            open_->complete({});
        }
        const bool ended =
            ready_ == data_channel_state::closing || ready_ == data_channel_state::closed;
        if (ended) {
            if (open_ != nullptr) {
                open_->fail(make_error_code(error::closed));
            }
            if (low_ != nullptr) {
                low_->fail(make_error_code(error::closed));
            }
        }
        if (std::exchange(low_arrived_, false)) {
            buffered_amount_low();
        }
        attempt();
        if (receive_ != nullptr && failure_) {
            receive_->fail(failure_);
        } else if (receive_ != nullptr && ready_ == data_channel_state::closed) {
            receive_->fail(make_error_code(error::closed));
        }
        if (ready_ == data_channel_state::closed && close_ != nullptr) {
            close_->complete({});
        }
    }

    void buffered_amount_low() {
        if (low_ != nullptr && buffered_amount() <= threshold_) {
            low_->complete({});
        }
    }

    // Reads the oldest message for the pending receive, if there is one.
    void attempt() {
        if (receive_ == nullptr || messages_.empty()) {
            return;
        }
        const message& front = messages_.front();
        if (const error_code ec = append_(target_, front)) {
            receive_->fail(ec);
            return;
        }
        const std::size_t size = front.bytes.size();
        const bool text = front.text;
        queued_ -= size + message_record_bytes;
        messages_.pop_front();
        receive_->complete({}, size, text);
    }

    void abort(error_code ec) {
        if (open_ != nullptr) {
            open_->fail(ec);
        }
        if (receive_ != nullptr) {
            receive_->fail(ec);
        }
        if (low_ != nullptr) {
            low_->fail(ec);
        }
    }

    void discard_messages() noexcept {
        accepting_ = false;
        messages_.clear();
        queued_ = 0;
    }

    // Leaves the bridge and sets the handlers to null, so that nothing reaches the state.
    void detach() {
        if (bridge_ != nullptr && key_ != 0) {
            bridge_->remove(std::exchange(key_, 0));
        }
        ++generation_;
        posted_ = false;
        if (!channel_.isUndefined()) {
            static_cast<void>(take(webcpp_webrtc_detach(channel_.as_handle(), true)));
        }
    }

    Executor executor_;
    Allocator allocator_;
    string_type label_;
    webcpp::asio::browser::bridge* bridge_ = nullptr;
    std::uint64_t key_ = 0;
    emscripten::val channel_ = emscripten::val::undefined();
    // The connection, whose SCTP transport says the peer's limit of a message.
    emscripten::val connection_;
    std::list<message, messages_allocator> messages_;
    std::size_t queued_ = 0;
    std::size_t max_message_size_ = 0;
    std::size_t max_buffered_amount_ = 0;
    std::size_t max_received_bytes_ = 0;
    std::size_t threshold_ = 0;
    data_channel_state ready_ = data_channel_state::connecting;
    error_code failure_;
    bool accepting_ = true;
    bool low_arrived_ = false;
    bool posted_ = false;
    std::uint64_t generation_ = 0;
    pending<>* open_ = nullptr;
    pending<>* close_ = nullptr;
    pending<>* low_ = nullptr;
    pending<std::size_t, bool>* receive_ = nullptr;
    void* target_ = nullptr;
    error_code (*append_)(void*, const message&) = nullptr;
};

}  // namespace unsupported_error or unsupported_ignore
}  // namespace webcpp::webrtc::detail::browser

#endif  // WEBCPP_WEBRTC_DETAIL_BROWSER_CHANNEL_HPP
