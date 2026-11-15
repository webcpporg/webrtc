// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Allocation in the browser (design.md, D12), as ../allocation_test.cpp checks it natively: two
// peers of one page made with a polymorphic allocator over a counting resource keep their state,
// their queues (the browser's receive queue among them) and their outputs there; an operation's
// state goes to its handler's associated allocator; and everything given is given back once the
// objects and the io_context are gone. The Jamfile builds it in both suspension variants, as the
// build asks and without exceptions; run.mjs waits for "browser webrtc allocation: ok".

#include <webcpp/webrtc.hpp>

#include "../pair.hpp"
#include "../require.hpp"
#include "page_pair.hpp"

#include <webcpp/asio/bind_allocator.hpp>  // lint-world: an operation's allocator
#include <webcpp/asio/buffer.hpp>          // lint-world: messages
#include <webcpp/asio/io_context.hpp>      // lint-world: the tests' event loop

#include <boost/core/lightweight_test.hpp>

#include <cstddef>
#include <iterator>
#include <map>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace webrtc = webcpp::webrtc;
namespace asio = webcpp::asio;
using webrtc::detail::error_code;

namespace {

// Counts what it gives and what is still out, and keeps the blocks it gave. A page has one thread,
// so plain counters do.
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations = 0;
    std::size_t live = 0;

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
        live += bytes;
        void* const p = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        blocks_.emplace(static_cast<const std::byte*>(p), bytes);
        return p;
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        live -= bytes;
        blocks_.erase(static_cast<const std::byte*>(p));
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

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
    counting_resource operations;
    {
        asio::io_context io;
        webrtc::runtime rt;
        const allocator a(&resource);
        const auto config = webcpp::test::page_configuration();
        auto first = connection::make(rt, io.get_executor(), config, webrtc::options(), a);
        auto second = connection::make(rt, io.get_executor(), config, webrtc::options(), a);
        webcpp::test::require(BOOST_TEST(first.has_value()));
        webcpp::test::require(BOOST_TEST(second.has_value()));
        const std::size_t made = resource.allocations;
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
        std::optional<webrtc::peer_connection_state> state;
        // An operation's state goes to its handler's allocator, not to the connection's.
        first->async_wait_state_change(asio::bind_allocator(
            allocator(&operations), [&](error_code ec, webrtc::peer_connection_state s) {
                BOOST_TEST(!ec);
                state = s;
            }));
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
        io.run_for(webcpp::test::exchange_limit);  // lint-run: the browser delivers the events
        BOOST_TEST(received == std::optional<std::size_t>(7));
        BOOST_TEST(text == "counted");
        BOOST_TEST(state.has_value());
        const auto description = first->local_description();
        webcpp::test::require(BOOST_TEST(description.has_value()));
        BOOST_TEST(description->sdp.get_allocator().resource() == &resource);
        BOOST_TEST_GT(resource.allocations, made);
        BOOST_TEST_GT(operations.allocations, 0u);
    }
    // The objects, the io_context and the handlers it held are gone, and so is what they took.
    BOOST_TEST_EQ(resource.live, 0u);
    BOOST_TEST_EQ(operations.live, 0u);
}

}  // namespace

int main() {
    state_queues_and_outputs_in_the_allocator();
    return webcpp::test::finish("browser webrtc allocation: ok");
}
