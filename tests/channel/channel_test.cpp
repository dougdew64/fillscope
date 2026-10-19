// The messages a frame source sends the viewer, and the socket that carries them.

#include "channel/message.h"
#include "channel/net.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <sys/socket.h>

#include <array>
#include <thread>

namespace fillscope {
namespace {

// Drains until `count` messages have arrived or a second has passed.
std::vector<Message> drain_until(MessageReceiver& receiver, std::size_t count) {
    std::vector<Message> messages;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (messages.size() < count && std::chrono::steady_clock::now() < deadline) {
        receiver.drain(messages);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return messages;
}

Frame frame(std::uint64_t index, std::int64_t price = 10'000) {
    return Frame{.index = index, .price = price, .qty = 5, .id = 3};
}

TEST(Message, EachKindRoundTrips) {
    const Message hello = SourceHello{.seed = 42, .frame_count = 1000};
    const Message one = Frame{.index = 7, .price = -3, .qty = 500, .id = 20};
    for (const Message& message : {hello, one}) {
        const EncodedMessage encoded = encode(message);
        const auto decoded = decode(encoded.view());
        ASSERT_TRUE(decoded.has_value()) << decoded.error();
        EXPECT_EQ(*decoded, message);
    }
}

TEST(Message, DecodeRejectsWhatIsNotAMessage) {
    // Too short for a header.
    EXPECT_FALSE(decode(std::array<std::byte, 3>{}).has_value());

    // An unknown kind.
    EncodedMessage unknown = encode(frame(0));
    unknown.bytes[0] = std::byte{99};
    EXPECT_FALSE(decode(unknown.view()).has_value());

    // A known kind, but the wrong size for it.
    EncodedMessage cut = encode(frame(0));
    cut.size -= 1;
    const auto result = decode(cut.view());
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("frame"), std::string::npos) << result.error();
}

TEST(Channel, EveryMessageSentArrivesInOrder) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver{listen_on(name)};
    const Fd sender = connect_to(name, 1, std::chrono::milliseconds{0});
    ASSERT_TRUE(sender);

    std::vector<Message> sent{SourceHello{.seed = 9, .frame_count = 100}};
    for (std::uint64_t i = 0; i < 100; ++i) {
        sent.emplace_back(frame(i, 10'000 + static_cast<std::int64_t>(i)));
    }
    for (const Message& message : sent) {
        ASSERT_TRUE(send_message(sender.get(), message));
    }
    EXPECT_EQ(drain_until(receiver, sent.size()), sent);
}

TEST(Channel, ReceiverReportsConnectAndDisconnect) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver{listen_on(name)};
    std::vector<Message> messages;
    receiver.drain(messages);
    EXPECT_FALSE(receiver.connected());

    {
        const Fd sender = connect_to(name, 1, std::chrono::milliseconds{0});
        ASSERT_TRUE(sender);
        receiver.drain(messages);
        EXPECT_TRUE(receiver.connected());
    }  // the sender closes here
    receiver.drain(messages);
    EXPECT_FALSE(receiver.connected());
    EXPECT_FALSE(receiver.error()) << "a sender leaving is not an error";
}

TEST(Channel, ReceiverAcceptsTheNextSenderAfterOneLeaves) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver{listen_on(name)};
    {
        const Fd first = connect_to(name, 1, std::chrono::milliseconds{0});
        ASSERT_TRUE(send_message(first.get(), frame(0, 1)));
        EXPECT_EQ(drain_until(receiver, 1).size(), 1u);
    }
    std::vector<Message> messages;
    receiver.drain(messages);  // notices the first sender has gone

    const Fd second = connect_to(name, 1, std::chrono::milliseconds{0});
    ASSERT_TRUE(send_message(second.get(), frame(0, 2)));
    const auto from_second = drain_until(receiver, 1);
    ASSERT_EQ(from_second.size(), 1u);
    EXPECT_EQ(std::get<Frame>(from_second[0]).price, 2);
}

TEST(Channel, ASenderOfSomethingElseIsDroppedAndTheReasonKept) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver{listen_on(name)};
    const Fd sender = connect_to(name, 1, std::chrono::milliseconds{0});
    ASSERT_TRUE(send_message(sender.get(), frame(0)));
    const std::array<std::byte, 5> garbage{};
    ASSERT_EQ(::send(sender.get(), garbage.data(), garbage.size(), MSG_NOSIGNAL), 5);
    ASSERT_TRUE(send_message(sender.get(), frame(1)));  // after the garbage: never delivered

    // Drain until the receiver gives up on the sender, rather than waiting for messages that won't come.
    std::vector<Message> messages;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!receiver.error() && std::chrono::steady_clock::now() < deadline) {
        receiver.drain(messages);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(messages.size(), 1u);
    EXPECT_FALSE(receiver.connected());
    ASSERT_TRUE(receiver.error());
    EXPECT_NE(receiver.error()->find("shorter than its header"), std::string::npos) << *receiver.error();
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
        MessageReceiver receiver{listen_on(name)};
        sender = connect_to(name, 1, std::chrono::milliseconds{0});
        ASSERT_TRUE(sender);
        std::vector<Message> messages;
        receiver.drain(messages);  // accepts
    }  // the receiver and its connection close here
    EXPECT_FALSE(send_message(sender.get(), frame(0)));
}

}  // namespace
}  // namespace fillscope
