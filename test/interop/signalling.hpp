// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// What the interop peers say to each other through the relay: one text frame of JSON per signal,
// the W3C's own dictionaries, so that a script in a browser would read them as they are. A
// description is an RTCSessionDescriptionInit, {"type", "sdp"}; a candidate is an
// RTCIceCandidateInit, {"candidate", "sdpMid", "sdpMLineIndex", "usernameFragment"}, whose
// optional members are null when unknown, and whose empty "candidate" ends the candidates. A
// signal is a description when it has "sdp", and a candidate otherwise.

#ifndef WEBCPP_TEST_WEBRTC_INTEROP_SIGNALLING_HPP
#define WEBCPP_TEST_WEBRTC_INTEROP_SIGNALLING_HPP

#include <webcpp/webrtc/types.hpp>

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>
#include <boost/system/errc.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace webcpp::test::interop {

namespace webrtc = webcpp::webrtc;

/** The W3C's names of `RTCSdpType`, in the order of `webrtc::sdp_type`. */
inline constexpr std::array<std::string_view, 4> sdp_type_names{
    "offer",
    "answer",
    "pranswer",
    "rollback",
};

/** The W3C's name of a description's type. */
inline std::string_view name_of(webrtc::sdp_type type) {
    return sdp_type_names.at(static_cast<std::size_t>(type));
}

/** A description as an RTCSessionDescriptionInit. */
inline std::string to_json(const webrtc::session_description_view& description) {
    boost::json::object signal;
    signal["type"] = name_of(description.type);
    signal["sdp"] = description.sdp;
    return boost::json::serialize(signal);
}

/** A candidate as an RTCIceCandidateInit; an empty `sdp_mid` or `username_fragment` is null. */
inline std::string to_json(const webrtc::ice_candidate_view& candidate) {
    boost::json::object signal;
    signal["candidate"] = candidate.candidate;
    signal["sdpMid"] = nullptr;
    if (candidate.sdp_mid) {
        signal["sdpMid"] = *candidate.sdp_mid;
    }
    signal["sdpMLineIndex"] = nullptr;
    if (candidate.sdp_m_line_index) {
        signal["sdpMLineIndex"] = *candidate.sdp_m_line_index;
    }
    signal["usernameFragment"] = nullptr;
    if (!candidate.username_fragment.empty()) {
        signal["usernameFragment"] = candidate.username_fragment;
    }
    return boost::json::serialize(signal);
}

/** A signal read from its JSON, owning its text: a description or a candidate. */
struct signal_message {
    /** A description's type; nothing for a candidate. */
    std::optional<webrtc::sdp_type> type;
    /** A description's SDP. */
    std::string sdp;
    /** A candidate's attribute line, empty at the end of the candidates. */
    std::string candidate;
    /** A candidate's media stream identification, when known. */
    std::optional<std::string> sdp_mid;
    /** A candidate's m-line index, when known. */
    std::optional<std::uint16_t> sdp_m_line_index;
    /** A candidate's ICE user name fragment, empty when unknown. */
    std::string username_fragment;

    /** Whether this signal is a description. */
    [[nodiscard]] bool is_description() const noexcept { return type.has_value(); }

    /** The description, valid while this signal is unchanged. */
    [[nodiscard]] webrtc::session_description_view description() const noexcept {
        return {.type = type.value_or(webrtc::sdp_type::offer), .sdp = sdp};
    }

    /** The candidate, valid while this signal is unchanged. */
    [[nodiscard]] webrtc::ice_candidate_view ice_candidate() const noexcept {
        return {
            .candidate = candidate,
            .sdp_mid = sdp_mid ? std::optional<std::string_view>(*sdp_mid) : std::nullopt,
            .sdp_m_line_index = sdp_m_line_index,
            .username_fragment = username_fragment,
        };
    }
};

namespace detail {

/** The code of a signal that is JSON but no W3C dictionary of the two. */
inline boost::system::error_code malformed() {
    return boost::system::errc::make_error_code(boost::system::errc::bad_message);
}

/** The string member `name` of `object`; nothing when it is null or absent. */
inline boost::system::result<std::optional<std::string>> optional_string(
    const boost::json::object& object, std::string_view name) {
    const auto* value = object.if_contains(name);
    if (value == nullptr || value->is_null()) {
        return std::optional<std::string>();
    }
    const auto* text = value->if_string();
    if (text == nullptr) {
        return malformed();
    }
    return std::optional<std::string>(std::string(text->data(), text->size()));
}

/** A description from an RTCSessionDescriptionInit. */
inline boost::system::result<signal_message> read_description(const boost::json::object& object) {
    auto sdp = optional_string(object, "sdp");
    const auto type = optional_string(object, "type");
    if (!sdp || !*sdp || !type || !*type) {
        return malformed();
    }
    signal_message result;
    for (std::size_t i = 0; i != sdp_type_names.size(); ++i) {
        if (**type == sdp_type_names.at(i)) {
            result.type = static_cast<webrtc::sdp_type>(i);
        }
    }
    if (!result.type) {
        return malformed();
    }
    result.sdp = std::move(**sdp);
    return result;
}

/** A candidate from an RTCIceCandidateInit. */
inline boost::system::result<signal_message> read_candidate(const boost::json::object& object) {
    auto candidate = optional_string(object, "candidate");
    auto mid = optional_string(object, "sdpMid");
    auto fragment = optional_string(object, "usernameFragment");
    if (!candidate || !*candidate || !mid || !fragment) {
        return malformed();
    }
    signal_message result;
    result.candidate = std::move(**candidate);
    result.sdp_mid = std::move(*mid);
    result.username_fragment = std::move(*fragment).value_or(std::string());
    if (const auto* index = object.if_contains("sdpMLineIndex");
        index != nullptr && !index->is_null()) {
        boost::system::error_code ec;
        const auto number = index->to_number<std::uint16_t>(ec);
        if (ec) {
            return malformed();
        }
        result.sdp_m_line_index = number;
    }
    return result;
}

}  // namespace detail

/** Reads a signal from its JSON; `bad_message` when it is no description and no candidate. */
inline boost::system::result<signal_message> parse_signal(std::string_view text) {
    boost::system::error_code ec;
    const auto value = boost::json::parse(text, ec);
    if (ec) {
        return ec;
    }
    const auto* object = value.if_object();
    if (object == nullptr) {
        return detail::malformed();
    }
    if (object->contains("sdp")) {
        return detail::read_description(*object);
    }
    return detail::read_candidate(*object);
}

}  // namespace webcpp::test::interop

#endif  // WEBCPP_TEST_WEBRTC_INTEROP_SIGNALLING_HPP
