#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>
#include <stdexcept>

namespace protocol {

    constexpr uint8_t CHANNEL_COMMAND = 0x01;
    constexpr uint8_t CHANNEL_STREAM  = 0x02;

    // Frame: [4 bytes big-endian length][payload]
    constexpr uint32_t MAX_FRAME_SIZE = 16 * 1024 * 1024;

    inline std::vector<uint8_t> encode_frame(const std::string_view payload) {
        if (payload.size() > MAX_FRAME_SIZE) {
            throw std::length_error("payload exceeds MAX_FRAME_SIZE");
        }

        const auto len = static_cast<uint32_t>(payload.size());
        std::vector<uint8_t> frame(4 + len);
        frame[0] = (len >> 24) & 0xFF;
        frame[1] = (len >> 16) & 0xFF;
        frame[2] = (len >> 8)  & 0xFF;
        frame[3] = (len)       & 0xFF;
        std::memcpy(frame.data() + 4, payload.data(), len);
        return frame;
    }

    inline void encode_frame(const std::string_view payload, std::vector<uint8_t>& out) {
        if (payload.size() > MAX_FRAME_SIZE) {
            throw std::length_error("payload exceeds MAX_FRAME_SIZE");
        }

        const auto len = static_cast<uint32_t>(payload.size());
        out.resize(4 + len);
        out[0] = (len >> 24) & 0xFF;
        out[1] = (len >> 16) & 0xFF;
        out[2] = (len >> 8)  & 0xFF;
        out[3] = (len)       & 0xFF;
        std::memcpy(out.data() + 4, payload.data(), len);
    }

    // Decodes a 4-byte big-endian length prefix.
    // Caller must ensure buf points to at least 4 readable bytes.
    inline uint32_t decode_length(const uint8_t* buf) {
        if (!buf) {
            throw std::invalid_argument("decode_length: null buffer");
        }
        return (static_cast<uint32_t>(buf[0]) << 24) |
               (static_cast<uint32_t>(buf[1]) << 16) |
               (static_cast<uint32_t>(buf[2]) << 8)  |
               (static_cast<uint32_t>(buf[3]));
    }

} // namespace protocol