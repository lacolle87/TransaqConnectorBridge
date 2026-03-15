#include <gtest/gtest.h>
#include "protocol.h"

#include <string>
#include <vector>
#include <cstdint>
#include <numeric>

// ============================================================
// encode_frame
// ============================================================

TEST(ProtocolEncode, SimplePayload) {
    std::string payload = "hello";
    auto frame = protocol::encode_frame(payload);

    // 4 bytes header + 5 bytes payload
    ASSERT_EQ(frame.size(), 9u);

    // Big-endian length = 5
    EXPECT_EQ(frame[0], 0x00);
    EXPECT_EQ(frame[1], 0x00);
    EXPECT_EQ(frame[2], 0x00);
    EXPECT_EQ(frame[3], 0x05);

    // Payload bytes
    EXPECT_EQ(frame[4], 'h');
    EXPECT_EQ(frame[5], 'e');
    EXPECT_EQ(frame[6], 'l');
    EXPECT_EQ(frame[7], 'l');
    EXPECT_EQ(frame[8], 'o');
}

TEST(ProtocolEncode, EmptyPayload) {
    auto frame = protocol::encode_frame("");

    ASSERT_EQ(frame.size(), 4u);
    EXPECT_EQ(frame[0], 0x00);
    EXPECT_EQ(frame[1], 0x00);
    EXPECT_EQ(frame[2], 0x00);
    EXPECT_EQ(frame[3], 0x00);
}

TEST(ProtocolEncode, SingleByte) {
    auto frame = protocol::encode_frame("X");

    ASSERT_EQ(frame.size(), 5u);
    EXPECT_EQ(frame[3], 0x01);
    EXPECT_EQ(frame[4], 'X');
}

TEST(ProtocolEncode, Length256_BigEndian) {
    // 256 bytes payload → length = 0x00000100
    std::string payload(256, 'A');
    auto frame = protocol::encode_frame(payload);

    ASSERT_EQ(frame.size(), 260u);
    EXPECT_EQ(frame[0], 0x00);
    EXPECT_EQ(frame[1], 0x00);
    EXPECT_EQ(frame[2], 0x01);
    EXPECT_EQ(frame[3], 0x00);
}

TEST(ProtocolEncode, Length65536_BigEndian) {
    // 65536 = 0x00010000
    std::string payload(65536, 'B');
    auto frame = protocol::encode_frame(payload);

    ASSERT_EQ(frame.size(), 65540u);
    EXPECT_EQ(frame[0], 0x00);
    EXPECT_EQ(frame[1], 0x01);
    EXPECT_EQ(frame[2], 0x00);
    EXPECT_EQ(frame[3], 0x00);
}

TEST(ProtocolEncode, XmlCommand) {
    std::string xml = R"(<command id="connect"><login>user</login></command>)";
    auto frame = protocol::encode_frame(xml);

    ASSERT_EQ(frame.size(), 4 + xml.size());

    // Extract payload back
    std::string extracted(frame.begin() + 4, frame.end());
    EXPECT_EQ(extracted, xml);
}

TEST(ProtocolEncode, Utf8Payload) {
    // Transaq protocol uses UTF-8
    std::string utf8 = u8"Привет мир";
    auto frame = protocol::encode_frame(utf8);

    auto len = static_cast<uint32_t>(utf8.size());
    ASSERT_EQ(frame.size(), 4 + len);

    std::string extracted(frame.begin() + 4, frame.end());
    EXPECT_EQ(extracted, utf8);
}

TEST(ProtocolEncode, BinaryPayload) {
    // Payload with null bytes and high bytes
    std::string payload;
    payload.push_back('\x00');
    payload.push_back('\xFF');
    payload.push_back('\x42');
    payload.push_back('\x00');

    auto frame = protocol::encode_frame(payload);

    ASSERT_EQ(frame.size(), 8u);
    EXPECT_EQ(frame[3], 0x04);
    EXPECT_EQ(frame[4], 0x00);
    EXPECT_EQ(frame[5], 0xFF);
    EXPECT_EQ(frame[6], 0x42);
    EXPECT_EQ(frame[7], 0x00);
}

TEST(ProtocolEncode, ThrowsOnOversizedPayload) {
    std::string huge(protocol::MAX_FRAME_SIZE + 1, 'X');
    EXPECT_THROW(protocol::encode_frame(huge), std::length_error);
}

// ============================================================
// decode_length
// ============================================================

TEST(ProtocolDecode, Zero) {
    uint8_t buf[] = {0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(protocol::decode_length(buf), 0u);
}

TEST(ProtocolDecode, One) {
    uint8_t buf[] = {0x00, 0x00, 0x00, 0x01};
    EXPECT_EQ(protocol::decode_length(buf), 1u);
}

TEST(ProtocolDecode, Value256) {
    uint8_t buf[] = {0x00, 0x00, 0x01, 0x00};
    EXPECT_EQ(protocol::decode_length(buf), 256u);
}

TEST(ProtocolDecode, Value65536) {
    uint8_t buf[] = {0x00, 0x01, 0x00, 0x00};
    EXPECT_EQ(protocol::decode_length(buf), 65536u);
}

TEST(ProtocolDecode, Value16M) {
    uint8_t buf[] = {0x01, 0x00, 0x00, 0x00};
    EXPECT_EQ(protocol::decode_length(buf), 16777216u);
}

TEST(ProtocolDecode, MaxUint32) {
    uint8_t buf[] = {0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(protocol::decode_length(buf), 0xFFFFFFFFu);
}

TEST(ProtocolDecode, ArbitraryValue) {
    // 0x12345678 = 305419896
    uint8_t buf[] = {0x12, 0x34, 0x56, 0x78};
    EXPECT_EQ(protocol::decode_length(buf), 0x12345678u);
}

TEST(ProtocolDecode, HighBitOnly) {
    uint8_t buf[] = {0x80, 0x00, 0x00, 0x00};
    EXPECT_EQ(protocol::decode_length(buf), 0x80000000u);
}

// ============================================================
// Roundtrip: encode → decode
// ============================================================

TEST(ProtocolRoundtrip, SmallPayload) {
    std::string original = "test123";
    auto frame = protocol::encode_frame(original);

    uint32_t decoded_len = protocol::decode_length(frame.data());
    EXPECT_EQ(decoded_len, original.size());

    std::string recovered(frame.begin() + 4, frame.begin() + 4 + decoded_len);
    EXPECT_EQ(recovered, original);
}

TEST(ProtocolRoundtrip, LargePayload) {
    // 100KB payload
    std::string original(100 * 1024, 'Z');
    auto frame = protocol::encode_frame(original);

    uint32_t decoded_len = protocol::decode_length(frame.data());
    EXPECT_EQ(decoded_len, original.size());

    std::string recovered(frame.begin() + 4, frame.begin() + 4 + decoded_len);
    EXPECT_EQ(recovered, original);
}

TEST(ProtocolRoundtrip, EmptyPayload) {
    auto frame = protocol::encode_frame("");

    uint32_t decoded_len = protocol::decode_length(frame.data());
    EXPECT_EQ(decoded_len, 0u);
}

TEST(ProtocolRoundtrip, AllByteValues) {
    // Payload with all 256 byte values
    std::string original(256, '\0');
    std::iota(original.begin(), original.end(), '\0');

    auto frame = protocol::encode_frame(original);
    uint32_t decoded_len = protocol::decode_length(frame.data());
    EXPECT_EQ(decoded_len, 256u);

    std::string recovered(frame.begin() + 4, frame.begin() + 4 + decoded_len);
    EXPECT_EQ(recovered, original);
}

TEST(ProtocolRoundtrip, MultipleFrames) {
    // Simulate multiple sequential frames in a buffer
    std::string msg1 = "<result success=\"true\"/>";
    std::string msg2 = "<server_status connected=\"true\"/>";
    std::string msg3 = "";

    auto f1 = protocol::encode_frame(msg1);
    auto f2 = protocol::encode_frame(msg2);
    auto f3 = protocol::encode_frame(msg3);

    // Concatenate all frames
    std::vector<uint8_t> wire;
    wire.insert(wire.end(), f1.begin(), f1.end());
    wire.insert(wire.end(), f2.begin(), f2.end());
    wire.insert(wire.end(), f3.begin(), f3.end());

    // Parse them back
    size_t offset = 0;

    uint32_t len1 = protocol::decode_length(wire.data() + offset);
    EXPECT_EQ(len1, msg1.size());
    std::string r1(wire.begin() + offset + 4, wire.begin() + offset + 4 + len1);
    EXPECT_EQ(r1, msg1);
    offset += 4 + len1;

    uint32_t len2 = protocol::decode_length(wire.data() + offset);
    EXPECT_EQ(len2, msg2.size());
    std::string r2(wire.begin() + offset + 4, wire.begin() + offset + 4 + len2);
    EXPECT_EQ(r2, msg2);
    offset += 4 + len2;

    uint32_t len3 = protocol::decode_length(wire.data() + offset);
    EXPECT_EQ(len3, 0u);
    offset += 4 + len3;

    EXPECT_EQ(offset, wire.size());
}

// ============================================================
// Constants
// ============================================================

TEST(ProtocolConstants, ChannelTypes) {
    EXPECT_EQ(protocol::CHANNEL_COMMAND, 0x01);
    EXPECT_EQ(protocol::CHANNEL_STREAM,  0x02);
    EXPECT_NE(protocol::CHANNEL_COMMAND, protocol::CHANNEL_STREAM);
}

TEST(ProtocolConstants, MaxFrameSize) {
    EXPECT_EQ(protocol::MAX_FRAME_SIZE, 16u * 1024 * 1024);
}
