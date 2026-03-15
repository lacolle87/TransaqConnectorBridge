#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <atomic>
#include <mutex>

class MessageRingBuffer {
public:
    explicit MessageRingBuffer(size_t capacity);

    bool push(const char* data, uint32_t len);
    bool push(const std::string& msg);

    bool pop(std::string& out);

    [[nodiscard]] bool   empty()          const;
    [[nodiscard]] size_t count_items()    const;
    [[nodiscard]] size_t capacity_bytes() const;
    [[nodiscard]] size_t used_bytes()     const;

private:
    static constexpr uint32_t SENTINEL = 0xFFFFFFFF;

    void     write_u32(size_t pos, uint32_t val);
    uint32_t read_u32(size_t pos) const;

    std::vector<uint8_t> buf_;
    size_t capacity_;

    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};

    alignas(64) std::atomic<size_t> count_{0};

    std::mutex push_mutex_;
};