// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// What the native target cannot honour (design.md, D14): a data channel's max_packet_life_time of
// 0, which libdatachannel's C API takes as 0 retransmissions, so that a peer would read
// maxRetransmits 0. Under the default policy create_data_channel fails with
// operation_not_supported and the connection goes on; under webcpp-webrtc-unsupported=ignore,
// which the test is built with too, the channel is created with 0 retransmissions, the platform's
// equivalent, which both of its ends read.

#include <webcpp/webrtc.hpp>

#include "attributes.hpp"
#include "pair.hpp"

#include <webcpp/asio/error.hpp>  // lint-world: Asio's codes

#include <boost/core/lightweight_test.hpp>

#include <array>

namespace webrtc = webcpp::webrtc;
using webcpp::test::pair;

namespace {

constexpr webrtc::data_channel_init zero_lifetime{.max_packet_life_time = 0};

#ifdef WEBCPP_WEBRTC_UNSUPPORTED_IGNORE

// The channel is created with 0 retransmissions, which its creator and its peer read.
void a_lifetime_of_zero() {
    pair p;
    const auto cases = std::to_array<webcpp::test::attribute_case>({
        {
            .init = zero_lifetime,
            .read = {.label = "zero lifetime", .max_retransmits = 0},
        },
    });
    webcpp::test::announced_attributes(p, cases);
}

#else

// The channel is refused, and the connection makes the channels it can honour, a lifetime of 1
// among them.
void a_lifetime_of_zero() {
    pair p;
    const auto refused = p.a->create_data_channel("zero lifetime", zero_lifetime);
    BOOST_TEST(refused.has_error() &&
               refused.error() == webcpp::asio::error::operation_not_supported);
    const auto one = p.a->create_data_channel("one", {.max_packet_life_time = 1});
    if (BOOST_TEST(one.has_value())) {
        BOOST_TEST(one->max_packet_life_time() == 1);
    }
    BOOST_TEST(p.a->create_data_channel("reliable").has_value());
}

#endif

}  // namespace

int main() {
    a_lifetime_of_zero();
    return boost::report_errors();
}
