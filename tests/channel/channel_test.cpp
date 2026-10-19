// The messages a frame source sends the viewer, and the socket that carries them.

#include "channel/message.h"
#include "channel/net.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <expected>
#include <optional>
#include <cstring>
#include <future>
#include <thread>

namespace fillscope {
namespace {

using namespace std::chrono_literals;

// Drains until `done(messages)` is true or a second has passed.
template <typename Done>
std::vector<Message> drain_until(MessageReceiver& receiver, Done done) {
    std::vector<Message> messages;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!done(messages) && std::chrono::steady_clock::now() < deadline) {
        receiver.drain(messages);
        std::this_thread::sleep_for(1ms);
    }
    return messages;
}

std::vector<Message> drain_count(MessageReceiver& receiver, std::size_t count) {
    return drain_until(receiver, [&](const std::vector<Message>& m) { return m.size() >= count; });
}

Frame frame(std::uint64_t index, std::int64_t price = 10'000) {
    return Frame{.index = index, .price = price, .qty = 5, .id = 3};
}

Fd connect(const std::string& name) {
    auto sender = connect_to(name, 1, 0ms);
    EXPECT_TRUE(sender.has_value()) << sender.error().message();
    return sender ? std::move(*sender) : Fd{};
}

// Sends raw bytes as one datagram, bypassing the encoding.
void send_raw(const Fd& sender, std::span<const std::byte> bytes) {
    ASSERT_EQ(::send(sender.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL), static_cast<ssize_t>(bytes.size()));
}

// ---- encoding ----

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

TEST(Message, TheWireLayoutIsKindThenPayloadAtOffsetEight) {
    // A change made consistently to both encode and decode would pass the round trip; the layout
    // itself is checked here, byte by byte.
    const Frame f{.index = 7, .price = -3, .qty = 500, .id = 20};
    const EncodedMessage encoded = encode(f);
    ASSERT_EQ(encoded.size, 8u + sizeof(Frame));
    std::uint32_t kind = 0;
    std::memcpy(&kind, encoded.bytes.data(), sizeof kind);
    EXPECT_EQ(kind, 2u);
    EXPECT_EQ(std::memcmp(encoded.bytes.data() + 8, &f, sizeof f), 0);

    const EncodedMessage hello = encode(SourceHello{.seed = 1, .frame_count = 2});
    ASSERT_EQ(hello.size, 8u + sizeof(SourceHello));
    std::memcpy(&kind, hello.bytes.data(), sizeof kind);
    EXPECT_EQ(kind, 1u);
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

// ---- the socket ----

TEST(Channel, EveryMessageSentArrivesInOrder) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    const Fd sender = connect(name);

    std::vector<Message> sent{SourceHello{.seed = 9, .frame_count = 100}};
    for (std::uint64_t i = 0; i < 100; ++i) {
        sent.emplace_back(frame(i, 10'000 + static_cast<std::int64_t>(i)));
    }
    for (const Message& message : sent) {
        ASSERT_TRUE(send_message(sender, message).has_value());
    }
    EXPECT_EQ(drain_count(receiver, sent.size()), sent);
}

TEST(Channel, ReceiverReportsConnectAndDisconnect) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    std::vector<Message> messages;
    EXPECT_FALSE(receiver.drain(messages).new_sender);
    EXPECT_FALSE(receiver.connected());

    {
        const Fd sender = connect(name);
        EXPECT_TRUE(receiver.drain(messages).new_sender);
        EXPECT_TRUE(receiver.connected());
        EXPECT_FALSE(receiver.drain(messages).new_sender) << "only the first drain after a connect reports it";
        // Read the acknowledgement, as the frame source does: a socket closed with unread data
        // reaches the receiver as a reset rather than an orderly end, and this is the orderly end.
        ASSERT_TRUE(wait_until_accepted(sender, 1s).has_value());
    }  // the sender closes here
    receiver.drain(messages);
    EXPECT_FALSE(receiver.connected());
    EXPECT_FALSE(receiver.error()) << "a sender leaving is not an error";
}

TEST(Channel, ReceiverAcceptsTheNextSenderAfterOneLeaves) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    {
        const Fd first = connect(name);
        ASSERT_TRUE(send_message(first, frame(0, 1)).has_value());
        EXPECT_EQ(drain_count(receiver, 1).size(), 1u);
    }
    std::vector<Message> messages;
    receiver.drain(messages);  // notices the first sender has gone

    const Fd second = connect(name);
    ASSERT_TRUE(send_message(second, frame(0, 2)).has_value());
    const auto from_second = drain_count(receiver, 1);
    ASSERT_EQ(from_second.size(), 1u);
    EXPECT_EQ(std::get<Frame>(from_second[0]).price, 2);
}

// Drains until the receiver drops its sender for sending something invalid.
std::vector<Message> drain_until_dropped(MessageReceiver& receiver) {
    return drain_until(receiver, [&](const std::vector<Message>&) { return receiver.error().has_value(); });
}

TEST(Channel, ASenderOfSomethingElseIsDroppedAndTheReasonKept) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    const Fd sender = connect(name);
    ASSERT_TRUE(send_message(sender, frame(0)).has_value());
    send_raw(sender, std::array<std::byte, 5>{});
    ASSERT_TRUE(send_message(sender, frame(1)).has_value());  // after the garbage: never delivered

    EXPECT_EQ(drain_until_dropped(receiver).size(), 1u);
    EXPECT_FALSE(receiver.connected());
    ASSERT_TRUE(receiver.error());
    EXPECT_NE(receiver.error()->find("shorter than its header"), std::string::npos) << *receiver.error();
}

TEST(Channel, AnOversizedDatagramIsRejectedNotCutToFit) {
    // A frame's encoding followed by extra bytes. Were the receive buffer exactly the largest
    // message's size, the datagram would be cut to that size and decode as a valid frame.
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    const Fd sender = connect(name);
    std::array<std::byte, kMaxMessageSize + 8> oversized{};
    const EncodedMessage valid = encode(frame(0));
    std::copy(valid.bytes.begin(), valid.bytes.begin() + static_cast<std::ptrdiff_t>(valid.size), oversized.begin());
    send_raw(sender, oversized);

    EXPECT_TRUE(drain_until_dropped(receiver).empty());
    ASSERT_TRUE(receiver.error());
    EXPECT_NE(receiver.error()->find("frame message of"), std::string::npos) << *receiver.error();
}

TEST(Channel, AnEmptyDatagramIsAnErrorNotADisconnect) {
    // recv returns 0 both for an empty datagram and for a sender that closed its end.
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    const Fd sender = connect(name);
    send_raw(sender, std::span<const std::byte>{});

    drain_until_dropped(receiver);
    ASSERT_TRUE(receiver.error());
    EXPECT_EQ(*receiver.error(), "an empty message");
}

TEST(Channel, ANewSenderClearsTheLastOnesError) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    {
        const Fd bad = connect(name);
        send_raw(bad, std::array<std::byte, 2>{});
        drain_until_dropped(receiver);
        ASSERT_TRUE(receiver.error());
    }
    const Fd good = connect(name);
    ASSERT_TRUE(send_message(good, frame(0)).has_value());
    EXPECT_EQ(drain_count(receiver, 1).size(), 1u);
    EXPECT_FALSE(receiver.error());
}

TEST(Channel, AnEmptyDatagramIsAnErrorEvenWhenTheSenderThenCloses) {
    // Closed, the sender looks as if it had just disconnected; the frame still queued after the
    // empty datagram says otherwise.
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    {
        const Fd sender = connect(name);
        ASSERT_TRUE(send_message(sender, frame(0)).has_value());
        send_raw(sender, std::span<const std::byte>{});
        ASSERT_TRUE(send_message(sender, frame(1)).has_value());
    }
    const auto messages = drain_until_dropped(receiver);
    EXPECT_EQ(messages.size(), 1u);
    ASSERT_TRUE(receiver.error());
    EXPECT_EQ(*receiver.error(), "an empty message");
}

TEST(Channel, ASecondSenderIsRefusedWhileOneIsConnected) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    const Fd first = connect(name);
    ASSERT_TRUE(send_message(first, frame(0, 1)).has_value());
    ASSERT_EQ(drain_count(receiver, 1).size(), 1u);

    const Fd second = connect(name);  // queued: the connect itself succeeds
    std::vector<Message> messages;
    receiver.drain(messages);  // refuses it
    // Its sends now fail, so it can't believe they reached the viewer. (The first may still
    // succeed, into the closed socket's buffer, before the close is seen.)
    std::expected<void, std::error_code> sent;
    for (int i = 0; i < 100 && sent; ++i) {
        sent = send_message(second, frame(0, 2));
    }
    ASSERT_FALSE(sent.has_value());
    EXPECT_TRUE(receiver_gone(sent.error())) << sent.error().message();
    EXPECT_TRUE(receiver.connected()) << "the first sender is still connected";
    ASSERT_TRUE(send_message(first, frame(1, 1)).has_value());
    EXPECT_EQ(drain_count(receiver, 1).size(), 1u);
}

TEST(Channel, ASenderLearnsWhetherItWasAcceptedRefusedOrIgnored) {
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    const Fd first = connect(name);
    EXPECT_EQ(wait_until_accepted(first, 10ms).error(), std::errc::timed_out) << "no drain yet";
    std::vector<Message> messages;
    receiver.drain(messages);
    EXPECT_TRUE(wait_until_accepted(first, 1s).has_value());

    const Fd second = connect(name);
    receiver.drain(messages);  // refuses it: the first is still connected
    EXPECT_EQ(wait_until_accepted(second, 1s).error(), std::errc::connection_refused);
}

TEST(Channel, ADrainTakesAtMostItsBound) {
    const std::string name = test::unique_socket_name();
    auto listener = listen_on(name);
    ASSERT_TRUE(listener.has_value());
    MessageReceiver receiver{std::move(*listener), 5};
    const Fd sender = connect(name);
    for (std::uint64_t i = 0; i < 12; ++i) {
        ASSERT_TRUE(send_message(sender, frame(i)).has_value());
    }
    std::vector<Message> messages;
    EXPECT_EQ(receiver.drain(messages).received, 5u);
    EXPECT_EQ(receiver.drain(messages).received, 5u);
    EXPECT_EQ(receiver.drain(messages).received, 2u);
}

TEST(Channel, AFailedAcceptIsReported) {
    // With no file descriptor left for it, accepting a waiting sender fails. That is a fault to
    // report, not "nobody waiting".
    const std::string name = test::unique_socket_name();
    MessageReceiver receiver = test::listen(name);
    const Fd sender = connect(name);
    rlimit original{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &original), 0);
    std::vector<Fd> filler;
    rlimit lowered = original;
    lowered.rlim_cur = 64;
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &lowered), 0);
    while (true) {
        Fd fd{::dup(0)};
        if (!fd) {
            break;  // EMFILE: the table is full
        }
        filler.push_back(std::move(fd));
    }
    std::vector<Message> messages;
    const DrainResult result = receiver.drain(messages);
    filler.clear();
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &original), 0);
    EXPECT_FALSE(result.new_sender);
    ASSERT_TRUE(receiver.error());
    EXPECT_NE(receiver.error()->find("can't accept a source"), std::string::npos) << *receiver.error();
    EXPECT_TRUE(receiver.drain(messages).new_sender) << "once descriptors are free, it is accepted";
    EXPECT_FALSE(receiver.error());
}

TEST(Channel, AnEmptyNameAndNoAttemptsAreRefused) {
    EXPECT_EQ(listen_on("").error(), std::errc::invalid_argument);
    EXPECT_EQ(connect_to("", 1, 0ms).error(), std::errc::invalid_argument);
    EXPECT_EQ(connect_to(test::unique_socket_name(), 0, 0ms).error(), std::errc::invalid_argument);
}

TEST(Channel, ConnectFailsWithNoListenerAndSaysWhy) {
    const auto result = connect_to(test::unique_socket_name(), 2, 1ms);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), std::errc::connection_refused) << result.error().message();
}

TEST(Channel, ConnectDoesNotBlockWhenTheListenersQueueIsFull) {
    // Nobody accepts, so the listener's queue fills; how many it holds is the kernel's business
    // (Linux holds one more than the backlog). Once it is full, a connect must fail promptly rather
    // than block until there is room.
    const std::string name = test::unique_socket_name();
    std::optional<MessageReceiver> receiver{test::listen(name)};
    std::vector<Fd> queued;
    for (int attempt = 0; attempt < 10; ++attempt) {
        auto pending = std::async(std::launch::async, [&] { return connect_to(name, 1, 0ms); });
        if (pending.wait_for(1s) != std::future_status::ready) {
            // Closing the listener makes the blocked connect fail, so the future's destructor,
            // which waits for it, returns and the test fails instead of hanging.
            receiver.reset();
            pending.wait();
            FAIL() << "connect blocked on attempt " << attempt;
        }
        auto connection = pending.get();
        if (!connection) {
            EXPECT_EQ(connection.error(), std::errc::resource_unavailable_try_again) << connection.error().message();
            return;
        }
        queued.push_back(std::move(*connection));
    }
    FAIL() << "the queue never filled";
}

TEST(Channel, OnlyOneListenerPerName) {
    const std::string name = test::unique_socket_name();
    const auto first = listen_on(name);
    EXPECT_TRUE(first.has_value());
    const auto second = listen_on(name);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), std::errc::address_in_use);
}

TEST(Channel, ANameTooLongIsRefusedRatherThanCut) {
    // Two names differing only past the limit would be the same address if names were cut.
    const std::string long_name(kMaxSocketNameLength + 1, 'x');
    const auto result = listen_on(long_name);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), std::errc::filename_too_long);
    std::string at_limit = test::unique_socket_name();
    at_limit.resize(kMaxSocketNameLength, 'y');  // unique, then padded to exactly the limit
    EXPECT_TRUE(listen_on(at_limit).has_value()) << "a name of exactly the limit is fine";
}

TEST(Channel, SendSaysWhenTheReceiverHasGone) {
    const std::string name = test::unique_socket_name();
    Fd sender;
    {
        MessageReceiver receiver = test::listen(name);
        sender = connect(name);
        std::vector<Message> messages;
        receiver.drain(messages);  // accepts
    }  // the receiver and its connection close here
    const auto sent = send_message(sender, frame(0));
    ASSERT_FALSE(sent.has_value());
    EXPECT_TRUE(receiver_gone(sent.error())) << sent.error().message();
}

}  // namespace
}  // namespace fillscope
