// The socket that carries frames to the viewer.

#include "channel/net.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <thread>

namespace fillscope {
namespace {

// Drains until `count` frames have arrived or a second has passed.
std::vector<Frame> drain_until(FrameReceiver& receiver, std::size_t count) {
    std::vector<Frame> frames;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (frames.size() < count && std::chrono::steady_clock::now() < deadline) {
        receiver.drain(frames);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return frames;
}

TEST(Channel, EveryFrameSentArrivesInOrder) {
    const std::string name = test::unique_socket_name();
    FrameReceiver receiver{listen_on(name)};
    const Fd sender = connect_to(name, 1, std::chrono::milliseconds{0});
    ASSERT_TRUE(sender);

    std::vector<Frame> sent;
    for (std::uint64_t i = 0; i < 100; ++i) {
        sent.push_back(Frame{i, 10'000 + static_cast<std::int64_t>(i), static_cast<std::uint32_t>(i % 7), 0});
        ASSERT_TRUE(send_frame(sender.get(), sent.back()));
    }
    EXPECT_EQ(drain_until(receiver, sent.size()), sent);
}

TEST(Channel, ReceiverReportsConnectAndDisconnect) {
    const std::string name = test::unique_socket_name();
    FrameReceiver receiver{listen_on(name)};
    std::vector<Frame> frames;
    receiver.drain(frames);
    EXPECT_FALSE(receiver.connected());

    {
        const Fd sender = connect_to(name, 1, std::chrono::milliseconds{0});
        ASSERT_TRUE(sender);
        receiver.drain(frames);
        EXPECT_TRUE(receiver.connected());
    }  // the sender closes here
    receiver.drain(frames);
    EXPECT_FALSE(receiver.connected());
}

TEST(Channel, ReceiverAcceptsTheNextSenderAfterOneLeaves) {
    const std::string name = test::unique_socket_name();
    FrameReceiver receiver{listen_on(name)};
    {
        const Fd first = connect_to(name, 1, std::chrono::milliseconds{0});
        ASSERT_TRUE(send_frame(first.get(), Frame{0, 1, 1, 0}));
        EXPECT_EQ(drain_until(receiver, 1).size(), 1u);
    }
    std::vector<Frame> frames;
    receiver.drain(frames);  // notices the first sender has gone

    const Fd second = connect_to(name, 1, std::chrono::milliseconds{0});
    ASSERT_TRUE(send_frame(second.get(), Frame{0, 2, 2, 0}));
    const auto from_second = drain_until(receiver, 1);
    ASSERT_EQ(from_second.size(), 1u);
    EXPECT_EQ(from_second[0].price, 2);
}

TEST(Channel, ConnectFailsWithNoListener) {
    EXPECT_FALSE(connect_to(test::unique_socket_name(), 2, std::chrono::milliseconds{1}));
}

TEST(Channel, OnlyOneListenerPerName) {
    const std::string name = test::unique_socket_name();
    const Fd first = listen_on(name);
    EXPECT_TRUE(first);
    EXPECT_FALSE(listen_on(name));
}

TEST(Channel, SendFailsOnceTheReceiverHasGone) {
    const std::string name = test::unique_socket_name();
    Fd sender;
    {
        FrameReceiver receiver{listen_on(name)};
        sender = connect_to(name, 1, std::chrono::milliseconds{0});
        ASSERT_TRUE(sender);
        std::vector<Frame> frames;
        receiver.drain(frames);  // accepts
    }  // the receiver and its connection close here
    EXPECT_FALSE(send_frame(sender.get(), Frame{0, 0, 0, 0}));
}

}  // namespace
}  // namespace fillscope
