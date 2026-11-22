// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_WEBRTC_DETAIL_BROWSER_CONNECTION_HPP
#define WEBCPP_WEBRTC_DETAIL_BROWSER_CONNECTION_HPP

#include <webcpp/webrtc/config.hpp>

#include <webcpp/webrtc/data_channel.hpp>
#include <webcpp/webrtc/detail/browser/census.hpp>
#include <webcpp/webrtc/detail/browser/channel.hpp>
#include <webcpp/webrtc/detail/browser/imports.hpp>
#include <webcpp/webrtc/detail/events.hpp>
#include <webcpp/webrtc/detail/operation.hpp>
#include <webcpp/webrtc/error.hpp>
#include <webcpp/webrtc/options.hpp>
#include <webcpp/webrtc/runtime.hpp>
#include <webcpp/webrtc/types.hpp>

#include <webcpp/asio/bind_allocator.hpp>  // lint-world: a notification in the object's allocator
#include <webcpp/asio/browser/bridge.hpp>  // lint-world: the browser's events
#include <webcpp/asio/cancellation_type.hpp>  // lint-world: what a cancellation can do
#include <webcpp/asio/error.hpp>              // lint-world: Asio's codes
#include <webcpp/asio/post.hpp>               // lint-world: a notification is posted

#include <emscripten/val.h>

#include <boost/system/result.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
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
 Whether what the browser cannot honour is ignored in this translation unit, rather than failing
 with `operation_not_supported` (design.md, D14).
*/
inline constexpr bool ignore_unsupported =
#ifdef WEBCPP_WEBRTC_UNSUPPORTED_IGNORE
    true;
#else
    false;
#endif

/**
 The state of a peer connection in the browser: its `RTCPeerConnection`, its channels, and what
 waits on it, on the object's executor.

 It is one registration of the io_context's bridge, which delivers every event of the connection
 and every settlement of its promises to `deliver`: arrivals are staged there, and one
 notification, posted for all those not yet drained, feeds the waits on the executor; a
 generation number makes a notification posted before `close` find nothing (design.md, D8).
*/
template <class Executor, class Allocator>
// The bridge indexes the registration, and the facade and the posted notifications share the
// state: two lifetimes of their own.
// NOLINTNEXTLINE(misc-multiple-inheritance)
class connection_state final
    : public webcpp::asio::browser::intrusive_registration,
      public std::enable_shared_from_this<connection_state<Executor, Allocator>> {
public:
    /** A channel's state. */
    using channel_type = channel_state<Executor, Allocator>;
    /** A channel. */
    using data_channel = basic_data_channel<Executor, Allocator>;
    /** A description, owned. */
    using session_description = basic_session_description<Allocator>;
    /** A candidate, owned. */
    using ice_candidate = basic_ice_candidate<Allocator>;
    /** The connection's text type. */
    using string_type = detail::string<Allocator>;

    /**
     What a cancellation of `async_add_ice_candidate` can do: terminal only, since the browser's
     promise cannot be called back and the candidate may still be added (design.md, D11).
    */
    static constexpr webcpp::asio::cancellation_type candidate_cancellation =
        webcpp::asio::cancellation_type::terminal;

    /** Makes the state of a connection not yet made in the browser. */
    connection_state(runtime_slot slot, Executor executor, const options& opts,
                     const Allocator& allocator)
        : slot_(std::move(slot)),
          executor_(std::move(executor)),
          allocator_(allocator),
          options_(opts),
          candidates_(opts.max_queued_candidates, allocator),
          channels_(opts.max_queued_channels, allocator),
          staged_(staged_allocator(allocator)),
          locals_(locals_allocator(allocator)),
          others_(others_allocator(allocator)),
          registry_(registry_allocator(allocator)) {}

    /** Not copyable: the bridge delivers to it by its address. */
    connection_state(const connection_state&) = delete;
    /** Not copy-assignable: the bridge delivers to it by its address. */
    connection_state& operator=(const connection_state&) = delete;
    /** Not movable: the bridge delivers to it by its address. */
    connection_state(connection_state&&) = delete;
    /** Not move-assignable: the bridge delivers to it by its address. */
    connection_state& operator=(connection_state&&) = delete;

    /** Closes the connection, aborting what waits, and leaves the bridge. */
    // Posting the aborted completions allocates, and a failure to allocate there ends the
    // program, as in any noexcept destructor that must complete operations.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~connection_state() { destroy(); }

    /**
     Makes the browser's connection and counts it in the page's census.

     @return `operation_not_supported` for a `bind_address` under the policy that refuses it;
     what the constructor threw, by its name (design.md, D10); nothing once made.
    */
    error_code open(const configuration& config) {
        // The browser chooses its own addresses: bind_address has no equivalent there.
        if (!config.bind_address.empty() && !ignore_unsupported) {
            return webcpp::asio::error::operation_not_supported;
        }
        const emscripten::val servers = emscripten::val::array();
        for (const ice_server& server : config.ice_servers) {
            const emscripten::val urls = emscripten::val::array();
            for (const std::string_view url : server.urls) {
                urls.call<void>("push", js_string(url));
            }
            emscripten::val entry = emscripten::val::object();
            entry.set("urls", urls);
            if (!server.username.empty()) {
                entry.set("username", js_string(server.username));
            }
            if (!server.credential.empty()) {
                entry.set("credential", js_string(server.credential));
            }
            servers.call<void>("push", entry);
        }
        emscripten::val native = emscripten::val::object();
        native.set("iceServers", servers);
        native.set("iceTransportPolicy",
                   config.policy == ice_transport_policy::relay ? "relay" : "all");
        bridge_ = &bridge_of(executor_);
        key_ = bridge_->add(*this);
        callback_ = bridge_->function(key_);
        const emscripten::val made = take(
            webcpp_webrtc_make(native.as_handle(), callback_.as_handle(), census().as_handle()));
        const emscripten::val failure = made["error"];
        if (!failure.isUndefined()) {
            detach();
            return error_of(failure);
        }
        pc_ = made["pc"];
        return {};
    }

    /** The object's executor. */
    [[nodiscard]] const Executor& get_executor() const noexcept { return executor_; }

    /** The object's allocator. */
    [[nodiscard]] const Allocator& get_allocator() const noexcept { return allocator_; }

    /** Creates and applies the implicit local description, an offer or an answer. */
    void set_local_description(pending<session_description>& op) {
        if (closed_) {
            op.fail(make_error_code(error::invalid_state));
            return;
        }
        const std::uint64_t id = next_id_++;
        op.link(locals_.emplace_back(promised<session_description>{id, nullptr, false}).op);
        webcpp_webrtc_set_local(pc_.as_handle(), callback_.as_handle(), static_cast<double>(id));
    }

    /** Applies the remote description `sdp` of type `type`. */
    void set_remote_description(sdp_type type, const string_type& sdp, pending<>& op) {
        if (closed_) {
            op.fail(make_error_code(error::invalid_state));
            return;
        }
        const std::uint64_t id = next_id_++;
        op.link(others_.emplace_back(promised<>{id, nullptr, false}).op);
        remote_in_flight_ = id;
        webcpp_webrtc_set_remote(pc_.as_handle(), static_cast<int>(type),
                                 js_string(sdp).as_handle(), callback_.as_handle(),
                                 static_cast<double>(id));
    }

    /**
     Adds the remote candidate `candidate`; the browser's own operations chain checks the remote
     description when the candidate's turn comes. An empty one, the end of the remote candidates,
     adds none: it completes at once, as natively, or with `error::invalid_state` when there is no
     remote description; while one is being applied, it completes once that one settles, as the
     chain would order it.
    */
    void add_ice_candidate(const string_type& candidate, const std::optional<string_type>& mid,
                           std::optional<std::uint16_t> index, const string_type& ufrag,
                           pending<>& op) {
        if (closed_) {
            op.fail(make_error_code(error::invalid_state));
            return;
        }
        if (candidate.empty()) {
            if (remote_in_flight_ != 0) {
                op.link(others_.emplace_back(promised<>{remote_in_flight_, nullptr, true}).op);
                return;
            }
            if (!has_remote_description()) {
                op.fail(make_error_code(error::invalid_state));
                return;
            }
            op.complete({});
            return;
        }
        emscripten::val init = emscripten::val::object();
        init.set("candidate", js_string(candidate));
        if (mid) {
            init.set("sdpMid", js_string(*mid));
        }
        if (index) {
            init.set("sdpMLineIndex", static_cast<int>(*index));
        }
        if (!ufrag.empty()) {
            init.set("usernameFragment", js_string(ufrag));
        }
        const std::uint64_t id = next_id_++;
        op.link(others_.emplace_back(promised<>{id, nullptr, false}).op);
        webcpp_webrtc_add_candidate(pc_.as_handle(), init.as_handle(), callback_.as_handle(),
                                    static_cast<double>(id));
    }

    /** Waits for the next local candidate, or for the end of gathering. */
    void wait_ice_candidate(pending<ice_candidate>& op) { candidates_.wait(op); }

    /** Waits for the connection's state to change. */
    void wait_state_change(pending<peer_connection_state>& op) { state_.wait(op); }

    /** Waits for the ICE state to change. */
    void wait_ice_state_change(pending<ice_connection_state>& op) { ice_.wait(op); }

    /** Waits for the next channel the peer opened. */
    void accept_data_channel(pending<data_channel>& op) { channels_.wait(op); }

    /** Creates a channel, the W3C's `createDataChannel`. */
    [[nodiscard]] boost::system::result<data_channel> create_data_channel(
        std::string_view label, const data_channel_init& init) {
        if (init.max_packet_life_time && init.max_retransmits) {
            return make_error_code(error::type_error);
        }
        if ((init.negotiated && !init.id) || (init.id && *init.id == 65535)) {
            return make_error_code(error::type_error);
        }
        if (label.size() > 65535 || init.protocol.size() > 65535) {
            return make_error_code(error::type_error);
        }
        if (closed_) {
            return make_error_code(error::invalid_state);
        }
        emscripten::val native = emscripten::val::object();
        native.set("ordered", init.ordered);
        if (init.max_packet_life_time) {
            native.set("maxPacketLifeTime", static_cast<int>(*init.max_packet_life_time));
        }
        if (init.max_retransmits) {
            native.set("maxRetransmits", static_cast<int>(*init.max_retransmits));
        }
        native.set("protocol", js_string(init.protocol));
        native.set("negotiated", init.negotiated);
        if (init.id) {
            native.set("id", static_cast<int>(*init.id));
        }
        const emscripten::val made = take(webcpp_webrtc_channel(
            pc_.as_handle(), js_string(label).as_handle(), native.as_handle()));
        const emscripten::val failure = made["error"];
        if (!failure.isUndefined()) {
            return error_of(failure);
        }
        return channel_access::make<data_channel>(adopt(made["channel"]));
    }

    /** The local description, with the candidates gathered so far, once one was set. */
    [[nodiscard]] std::optional<session_description> local_description() const {
        if (pc_.isUndefined()) {
            return std::nullopt;
        }
        const emscripten::val local = take(webcpp_webrtc_local_description(pc_.as_handle()));
        if (local.isNull()) {
            return std::nullopt;
        }
        const int type = local["type"].as<int>();
        if (type < 0) {
            return std::nullopt;
        }
        session_description description{static_cast<sdp_type>(type), string_type(text_allocator())};
        js_copy(local["sdp"], description.sdp);
        return description;
    }

    /** Whether a remote description was applied. */
    [[nodiscard]] bool has_remote_description() const {
        return !pc_.isUndefined() && webcpp_webrtc_has_remote(pc_.as_handle());
    }

    /**
     The largest message a channel sends: the smaller of `options::max_message_size` and the
     SCTP transport's `maxMessageSize` (`Infinity` for no limit), the limit each channel's sends
     check; nothing while the connection has no SCTP transport (`sctp` is null).
    */
    [[nodiscard]] std::optional<std::size_t> max_message_size() const {
        if (pc_.isUndefined()) {
            return std::nullopt;
        }
        const double peer = webcpp_webrtc_peer_max_message_size(pc_.as_handle());
        if (!(peer > 0)) {
            return std::nullopt;
        }
        if (peer >= static_cast<double>(options_.max_message_size)) {
            return options_.max_message_size;
        }
        return static_cast<std::size_t>(peer);
    }

    /** The connection's state. */
    [[nodiscard]] peer_connection_state state() const noexcept { return state_.current(); }

    /** The ICE state. */
    [[nodiscard]] ice_connection_state ice_state() const noexcept { return ice_.current(); }

    /**
     Closes the connection, as the W3C's `close()`: no state change is delivered, what waits on
     the connection ends with `operation_aborted`, its channels close with `error::closed`, and
     the handlers are set to null.
    */
    void close() {
        if (closed_) {
            return;
        }
        closed_ = true;
        discard_staged();
        const error_code aborted(webcpp::asio::error::operation_aborted);
        state_.close(peer_connection_state::closed, aborted);
        ice_.close(ice_connection_state::closed, aborted);
        candidates_.close(aborted);
        // Channels not yet accepted go with the queue, and close.
        channels_.close(aborted);
        fail_all(locals_, aborted);
        fail_all(others_, aborted);
        // The browser closes its channels without an event: each says so to what waits on it.
        for (const auto& weak : registry_) {
            if (const auto channel = weak.lock()) {
                channel->connection_closed();
            }
        }
        registry_.clear();
        detach();
        if (!pc_.isUndefined()) {
            static_cast<void>(take(webcpp_webrtc_close(pc_.as_handle())));
        }
    }

    /** Closes the connection and lets the browser's object go, to the garbage collector. */
    void destroy() {
        close();
        pc_ = emscripten::val::undefined();
        slot_.reset();
    }

    /**
     Stages what the browser delivered and posts one notification for the arrivals not yet
     drained; it runs no handler. A candidate past the room of the queue is dropped here, and a
     channel past it closed, at once, so that the staging is bounded whatever the executor does.

     @param event The event, as the imports deliver it.
     @return False: the registration stays until the connection closes.
    */
    bool deliver(const emscripten::val& event) override {
        if (closed_) {
            return false;
        }
        switch (static_cast<event_kind>(kind_of(event))) {
            case event_kind::connection_state: {
                const int value = event["state"].as<int>();
                if (value < 0 || value > static_cast<int>(peer_connection_state::closed)) {
                    return false;
                }
                staged_state_ = static_cast<peer_connection_state>(value);
                break;
            }
            case event_kind::ice_state: {
                const int value = event["state"].as<int>();
                if (value < 0 || value > static_cast<int>(ice_connection_state::closed)) {
                    return false;
                }
                staged_ice_ = static_cast<ice_connection_state>(value);
                break;
            }
            case event_kind::candidate:
                if (!event["end"].as<bool>()) {
                    // A candidate past the bound stays in the local description.
                    if (staged_counted_ >= candidates_.room()) {
                        return false;
                    }
                    ++staged_counted_;
                }
                staged_.push_back(arrival{event_kind::candidate, event, nullptr});
                break;
            case event_kind::channel: arrived(event["channel"]); break;
            case event_kind::settled:
                staged_.push_back(arrival{event_kind::settled, event, nullptr});
                break;
            default: return false;
        }
        notify();
        return false;
    }

    /**
     The io_context's shutdown: what waits is destroyed without its handler being called, as
     Asio's own operations are, and the browser's connection is closed.
    */
    void shutdown() noexcept override {
        // A handler destroyed here may hold the last owner of the connection: the state lives
        // until this returns, and the handlers are destroyed after every change of it, as Asio's
        // own services destroy theirs. Closed, the connection refuses what a handler's
        // destruction asks of it meanwhile.
        const auto keep = this->weak_from_this().lock();
        bridge_ = nullptr;
        key_ = 0;
        closed_ = true;
        discard_staged();
        registry_.clear();
        callback_ = emscripten::val::undefined();
        if (!pc_.isUndefined()) {
            static_cast<void>(take(webcpp_webrtc_detach(pc_.as_handle(), false)));
            static_cast<void>(take(webcpp_webrtc_close(pc_.as_handle())));
        }
        state_.discard();
        ice_.discard();
        candidates_.discard();
        channels_.discard();
        discard_all(locals_);
        discard_all(others_);
    }

private:
    /** An operation that follows a promise of the browser, by the id its settlement carries. */
    template <class... Values>
    struct promised {
        std::uint64_t id = 0;
        // Cleared by the operation when it ends, a cancellation included.
        pending<Values...>* op = nullptr;
        // The end of the remote candidates, waiting for the remote description `id` settles.
        bool end = false;
    };

    /**
     A candidate, a channel or a settlement, staged in the order it arrived: a settlement the
     browser delivered before a candidate completes its operation first.
    */
    struct arrival {
        event_kind kind = event_kind::candidate;
        // The event of a candidate or of a settlement.
        emscripten::val event;
        // The state of a channel, which took the browser's channel over as it arrived.
        std::shared_ptr<channel_type> channel;
    };

    using staged_allocator = std::allocator_traits<Allocator>::template rebind_alloc<arrival>;
    using locals_allocator =
        std::allocator_traits<Allocator>::template rebind_alloc<promised<session_description>>;
    using others_allocator = std::allocator_traits<Allocator>::template rebind_alloc<promised<>>;
    using registry_allocator =
        std::allocator_traits<Allocator>::template rebind_alloc<std::weak_ptr<channel_type>>;
    using text_allocator_type = string_type::allocator_type;

    [[nodiscard]] text_allocator_type text_allocator() const {
        return text_allocator_type(allocator_);
    }

    // Posts one notification for the arrivals not yet drained, which a later generation finds
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

    // Feeds the staged arrivals to the waits, on the executor.
    void drain() {
        posted_ = false;
        if (const auto changed = std::exchange(staged_state_, std::nullopt)) {
            state_.update(*changed);
        }
        if (const auto ice_changed = std::exchange(staged_ice_, std::nullopt)) {
            ice_.update(*ice_changed);
        }
        staged_counted_ = 0;
        staged_channels_ = 0;
        // In the order they arrived, whatever their kind.
        while (!staged_.empty()) {
            arrival next = std::move(staged_.front());
            staged_.pop_front();
            switch (next.kind) {
                case event_kind::candidate: gathered(next.event); break;
                case event_kind::channel:
                    // Past the backlog, the refused channel is destroyed here, which closes it.
                    static_cast<void>(channels_.push(
                        channel_access::make<data_channel>(std::move(next.channel))));
                    break;
                default: settle(next.event); break;
            }
        }
    }

    // A local candidate, or the end of gathering, which the bound keeps a place for.
    void gathered(const emscripten::val& event) {
        const bool end = event["end"].as<bool>();
        ice_candidate candidate{
            string_type(text_allocator()),
            string_type(text_allocator()),
            std::nullopt,
            string_type(text_allocator()),
        };
        if (!end) {
            js_copy(event["candidate"], candidate.candidate);
            const emscripten::val mid = event["sdpMid"];
            if (mid.isString()) {
                js_copy(mid, candidate.sdp_mid);
            }
            const emscripten::val index = event["sdpMLineIndex"];
            if (index.isNumber()) {
                candidate.sdp_m_line_index = static_cast<std::uint16_t>(index.as<int>());
            }
            const emscripten::val ufrag = event["usernameFragment"];
            if (ufrag.isString()) {
                js_copy(ufrag, candidate.username_fragment);
            }
        }
        // A candidate past the bound is dropped here; it is still in local_description().
        static_cast<void>(candidates_.push(std::move(candidate), end));
    }

    // A promise settled: the operation that follows it completes, unless it ended already.
    void settle(const emscripten::val& event) {
        const auto id = static_cast<std::uint64_t>(event["id"].as<double>());
        if (event["local"].as<bool>()) {
            settle_local(id, event);
        } else {
            settle_other(id, event);
        }
    }

    // A local description's promise settled.
    void settle_local(std::uint64_t id, const emscripten::val& event) {
        const auto found = find(locals_, id);
        if (found == locals_.end()) {
            return;
        }
        if (found->op != nullptr) {
            const bool ok = event["ok"].as<bool>();
            const int type = ok ? event["type"].as<int>() : -1;
            if (type < 0) {
                found->op->fail(ok ? make_error_code(error::operation_error)
                                   : error_of(event["name"]));
            } else {
                session_description description{
                    static_cast<sdp_type>(type),
                    string_type(text_allocator()),
                };
                js_copy(event["sdp"], description.sdp);
                found->op->complete({}, std::move(description));
            }
        }
        locals_.erase(found);
    }

    // A remote description's or a candidate's promise settled: its operation completes, then the
    // ends of the remote candidates that waited for it, a remote description's.
    void settle_other(std::uint64_t id, const emscripten::val& event) {
        if (remote_in_flight_ == id) {
            remote_in_flight_ = 0;
        }
        const bool ok = event["ok"].as<bool>();
        for (auto it = others_.begin(); it != others_.end();) {
            if (it->id != id) {
                ++it;
                continue;
            }
            if (it->op == nullptr) {
                // Cancelled already.
            } else if (it->end) {
                finish_end(*it->op);
            } else if (ok) {
                it->op->complete({});
            } else {
                it->op->fail(error_of(event["name"]));
            }
            it = others_.erase(it);
        }
    }

    // The end of the remote candidates, once the remote description it waited for settled.
    void finish_end(pending<>& op) {
        if (has_remote_description()) {
            op.complete({});
        } else {
            op.fail(make_error_code(error::invalid_state));
        }
    }

    template <class List>
    static auto find(List& list, std::uint64_t id) {
        auto it = list.begin();
        while (it != list.end() && it->id != id) {
            ++it;
        }
        return it;
    }

    template <class List>
    static void fail_all(List& list, error_code ec) {
        for (const auto& entry : list) {
            if (entry.op != nullptr) {
                entry.op->fail(ec);
            }
        }
        list.clear();
    }

    template <class List>
    static void discard_all(List& list) noexcept {
        for (const auto& entry : list) {
            if (entry.op != nullptr) {
                entry.op->discard();
            }
        }
        list.clear();
    }

    // A channel the peer opened: its state takes it over at once, so that nothing it delivers is
    // lost, unless the backlog has no room, when it is closed.
    void arrived(const emscripten::val& channel) {
        if (staged_channels_ >= channels_.room()) {
            static_cast<void>(take(webcpp_webrtc_close(channel.as_handle())));
            return;
        }
        staged_.push_back(
            arrival{event_kind::channel, emscripten::val::undefined(), adopt(channel)});
        ++staged_channels_;
    }

    std::shared_ptr<channel_type> adopt(const emscripten::val& channel) {
        auto state =
            std::allocate_shared<channel_type>(allocator_, executor_, allocator_, options_, pc_);
        state->attach(channel);
        std::erase_if(registry_, [](const auto& weak) { return weak.expired(); });
        registry_.push_back(state);
        return state;
    }

    // Drops what is staged; a channel never accepted goes, and closes.
    void discard_staged() {
        ++generation_;
        posted_ = false;
        staged_state_.reset();
        staged_ice_.reset();
        staged_counted_ = 0;
        staged_channels_ = 0;
        staged_.clear();
    }

    // Leaves the bridge and sets the handlers to null, so that nothing reaches the state.
    void detach() {
        if (bridge_ != nullptr && key_ != 0) {
            bridge_->remove(std::exchange(key_, 0));
        }
        callback_ = emscripten::val::undefined();
        if (!pc_.isUndefined()) {
            static_cast<void>(take(webcpp_webrtc_detach(pc_.as_handle(), false)));
        }
    }

    runtime_slot slot_;
    Executor executor_;
    Allocator allocator_;
    options options_;
    webcpp::asio::browser::bridge* bridge_ = nullptr;
    std::uint64_t key_ = 0;
    emscripten::val callback_ = emscripten::val::undefined();
    emscripten::val pc_ = emscripten::val::undefined();
    bool closed_ = false;
    bool posted_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t next_id_ = 1;
    // The remote description being applied, 0 for none.
    std::uint64_t remote_in_flight_ = 0;
    state_watch<peer_connection_state> state_;
    state_watch<ice_connection_state> ice_;
    event_queue<ice_candidate, Allocator> candidates_;
    event_queue<data_channel, Allocator> channels_;
    std::optional<peer_connection_state> staged_state_;
    std::optional<ice_connection_state> staged_ice_;
    // The candidates that are not exempt and the channels staged, which the bounds count.
    std::size_t staged_counted_ = 0;
    std::size_t staged_channels_ = 0;
    std::deque<arrival, staged_allocator> staged_;
    std::list<promised<session_description>, locals_allocator> locals_;
    std::list<promised<>, others_allocator> others_;
    std::vector<std::weak_ptr<channel_type>, registry_allocator> registry_;
};

}  // namespace unsupported_error or unsupported_ignore
}  // namespace webcpp::webrtc::detail::browser

#endif  // WEBCPP_WEBRTC_DETAIL_BROWSER_CONNECTION_HPP
