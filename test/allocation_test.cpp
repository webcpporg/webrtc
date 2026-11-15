// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Allocation (design.md, D12): a connection made with a polymorphic allocator over a counting
// resource keeps its state, its queues and its outputs there, and nothing of that resource is
// touched on libdatachannel's threads (spec: "Allocation: run-time objects use the object's
// allocator", "Semantics: native threads post to the executor").

#include <webcpp/webrtc.hpp>

#include "pair.hpp"
#include "require.hpp"

#include <webcpp/asio/buffer.hpp>      // lint-world: messages
#include <webcpp/asio/io_context.hpp>  // lint-world: the tests' event loop

#include <boost/core/lightweight_test.hpp>

#include <atomic>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;
using webrtc::detail::error_code;

namespace {

// Counts what it gives, and what it gives on a thread other than the one that made it, which is
// the thread that runs the io_context; and keeps the blocks it gave on that thread, which is the
// only one that asks whether it gave one.
class counting_resource final : public std::pmr::memory_resource {
public:
    std::atomic<std::size_t> allocations{0};
    std::atomic<std::size_t> foreign{0};

    // Whether `p` points into a block it gave and that was not given back.
    [[nodiscard]] bool gave(const void* p) const {
        const auto* address = static_cast<const std::byte*>(p);
        const auto after = blocks_.upper_bound(address);
        return after != blocks_.begin() &&
               address < std::prev(after)->first + std::prev(after)->second;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        void* const p = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        if (std::this_thread::get_id() != owner_) {  // lint-world: which thread allocates
            ++foreign;
        } else {
            blocks_.emplace(static_cast<const std::byte*>(p), bytes);
        }
        return p;
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        if (std::this_thread::get_id() == owner_) {  // lint-world: which thread gives back
            blocks_.erase(static_cast<const std::byte*>(p));
        }
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::thread::id owner_ = std::this_thread::get_id();  // lint-world: which thread allocates
    std::map<const std::byte*, std::size_t> blocks_;
};

// Checks that `text` reads `expected`, from a block `resource` gave.
void in_the_allocator(const counting_resource& resource, std::string_view text,
                      std::string_view expected) {
    BOOST_TEST_EQ(text, expected);
    BOOST_TEST(resource.gave(text.data()));
}

using allocator = std::pmr::polymorphic_allocator<std::byte>;
using connection = webrtc::basic_peer_connection<asio::any_io_executor, allocator>;
using channel = connection::data_channel;

void state_queues_and_outputs_in_the_allocator() {
    counting_resource resource;
    asio::io_context io;
    webrtc::runtime rt;
    const allocator a(&resource);
    auto first = connection::make(rt, io.get_executor(), webcpp::test::loopback_configuration(),
                                  webrtc::options(), a);
    auto second = connection::make(rt, io.get_executor(), webcpp::test::loopback_configuration(),
                                   webrtc::options(), a);
    webcpp::test::require(BOOST_TEST(first.has_value()));
    webcpp::test::require(BOOST_TEST(second.has_value()));
    const std::size_t made = resource.allocations.load();
    BOOST_TEST_GT(made, 0u);

    webcpp::test::trickle<connection> ab(*first, *second);
    webcpp::test::trickle<connection> ba(*second, *first);
    // A protocol longer than a string holds in place, so that both ends allocate its text.
    const std::string_view protocol = "webcpp.allocation.a-protocol-held-in-the-allocator";
    auto created = first->create_data_channel("allocated", {.protocol = protocol});
    webcpp::test::require(BOOST_TEST(created.has_value()));
    in_the_allocator(resource, created->protocol(), protocol);
    std::optional<channel> remote;
    std::string text;
    auto buffer = asio::dynamic_buffer(text);
    std::optional<std::size_t> received;
    created->async_wait_open([&](error_code ec) {
        BOOST_TEST(!ec);
        BOOST_TEST(!created->send_text("counted"));
    });
    second->async_accept_data_channel([&](error_code ec, channel c) {
        if (!BOOST_TEST(!ec)) {
            return;
        }
        remote.emplace(std::move(c));
        BOOST_TEST(remote->label() == "allocated");
        in_the_allocator(resource, remote->protocol(), protocol);
        remote->async_receive(buffer, [&](error_code ec, std::size_t n, bool) {
            BOOST_TEST(!ec);
            received = n;
            first->close();
            second->close();
        });
    });
    webcpp::test::negotiate(*first, *second, ab, ba, [](error_code ec) { BOOST_TEST(!ec); });
    io.run_for(webcpp::test::exchange_limit);  // lint-run: libdatachannel posts the peers' events
    BOOST_TEST(received == std::optional<std::size_t>(7));
    const auto description = first->local_description();
    webcpp::test::require(BOOST_TEST(description.has_value()));
    BOOST_TEST(description->sdp.get_allocator().resource() == &resource);
    BOOST_TEST_GT(resource.allocations.load(), made);
    BOOST_TEST_EQ(resource.foreign.load(), 0u);
}

}  // namespace

int main() {
    state_queues_and_outputs_in_the_allocator();
    return boost::report_errors();
}
