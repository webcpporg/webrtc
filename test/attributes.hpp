// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A channel's attributes, the W3C's `ordered`, `maxPacketLifeTime`, `maxRetransmits`, `protocol`
// and `negotiated`, as its creator reads them and as the peer that accepts it reads them from the
// creator's announcement. Shared by native_test.cpp and the browser lane's browser_test.cpp, each
// over its own pair of peers.

#ifndef WEBCPP_TEST_WEBRTC_ATTRIBUTES_HPP
#define WEBCPP_TEST_WEBRTC_ATTRIBUTES_HPP

#include <webcpp/webrtc.hpp>

#include "require.hpp"

#include <boost/core/lightweight_test.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::test {

/** What a program reads of a channel: its label, and the attributes it was created with. */
struct attributes {
    /** The label, which names the channel when a check fails. */
    std::string label{};
    /** `ordered()`. */
    bool ordered = true;
    /** `max_packet_life_time()`. */
    std::optional<std::uint16_t> max_packet_life_time{};
    /** `max_retransmits()`. */
    std::optional<std::uint16_t> max_retransmits{};
    /** `protocol()`. */
    std::string protocol{};
    /** `negotiated()`. */
    bool negotiated = false;

    /** Whether every member is equal. */
    bool operator==(const attributes& other) const = default;
};

/** Prints `a` on one line, for the message of a failed check. */
inline std::ostream& operator<<(std::ostream& out, const attributes& a) {
    const auto limit = [](std::optional<std::uint16_t> value) {
        return value ? std::to_string(*value) : std::string("none");
    };
    return out << '{' << a.label << ": ordered " << a.ordered << ", max_packet_life_time "
               << limit(a.max_packet_life_time) << ", max_retransmits " << limit(a.max_retransmits)
               << ", protocol '" << a.protocol << "', negotiated " << a.negotiated << '}';
}

/** What `channel` reads. */
inline attributes attributes_of(const webrtc::data_channel& channel) {
    return {
        .label = std::string(channel.label()),
        .ordered = channel.ordered(),
        .max_packet_life_time = channel.max_packet_life_time(),
        .max_retransmits = channel.max_retransmits(),
        .protocol = std::string(channel.protocol()),
        .negotiated = channel.negotiated(),
    };
}

/** A channel of `announced_attributes`: how it is created, and what both of its ends read. */
struct attribute_case {
    /** How it is created; never negotiated, since a negotiated channel is never accepted. */
    webrtc::data_channel_init init{};
    /** What its creator reads, and the peer that accepts it, its label among it. */
    attributes read{};
};

/**
 Creates a channel from `p.a` for each case, labelled as its `read` says, and checks what each
 reads at once; then accepts each at `p.b`, checks that it reads what its creator read, and
 closes both peers once every channel was accepted.
*/
template <class Pair>
void announced_attributes(Pair& p, std::span<const attribute_case> cases) {
    std::vector<webrtc::data_channel> created;
    for (const attribute_case& c : cases) {
        auto made = p.a->create_data_channel(c.read.label, c.init);
        require(BOOST_TEST(made.has_value()));
        BOOST_TEST_EQ(attributes_of(*made), c.read);
        created.push_back(std::move(*made));
    }
    std::vector<webrtc::data_channel> accepted;
    std::function<void()> accept = [&] {
        p.b->async_accept_data_channel([&](webrtc::detail::error_code ec, webrtc::data_channel dc) {
            if (!BOOST_TEST(!ec)) {
                return;
            }
            const auto it = std::ranges::find_if(
                cases, [&dc](const attribute_case& c) { return c.read.label == dc.label(); });
            if (BOOST_TEST(it != cases.end())) {
                BOOST_TEST_EQ(attributes_of(dc), it->read);
            }
            accepted.push_back(std::move(dc));
            if (accepted.size() < cases.size()) {
                accept();
                return;
            }
            p.a->close();
            p.b->close();
        });
    };
    accept();
    p.negotiate([](webrtc::detail::error_code ec) { BOOST_TEST(!ec); });
    p.run();
    BOOST_TEST_EQ(accepted.size(), cases.size());
}

}  // namespace webcpp::test

#endif  // WEBCPP_TEST_WEBRTC_ATTRIBUTES_HPP
