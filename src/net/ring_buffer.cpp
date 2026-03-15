#include "ring_buffer.h"
#include <cstring>
#include <algorithm>

MessageRingBuffer::MessageRingBuffer(const size_t capacity)
    : buf_(capacity),
      capacity_(capacity) {}

bool MessageRingBuffer::push(const char* data, const uint32_t len) {
    std::lock_guard<std::mutex> lock(push_mutex_);

    const size_t entry_size = sizeof(uint32_t) + len;

    if (entry_size + sizeof(uint32_t) > capacity_) return false;

    size_t w = write_pos_.load(std::memory_order_relaxed);
    const size_t r = read_pos_.load(std::memory_order_acquire);

    if (w + entry_size + sizeof(uint32_t) > capacity_) {
        if (w < r) return false;

        if (entry_size >= r) return false;

        write_u32(w, SENTINEL);
        w = 0;

    } else if (w < r) {
        if (w + entry_size >= r) return false;
    }

    write_u32(w, len);
    std::memcpy(buf_.data() + w + sizeof(uint32_t), data, len);

    write_pos_.store(w + entry_size, std::memory_order_release);
    count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool MessageRingBuffer::push(const std::string& msg) {
    return push(msg.data(), static_cast<uint32_t>(msg.size()));
}

bool MessageRingBuffer::pop(std::string& out) {
    size_t r = read_pos_.load(std::memory_order_relaxed);

    if (const size_t w = write_pos_.load(std::memory_order_acquire); r == w) return false;

    uint32_t len = read_u32(r);

    if (len == SENTINEL) {
        r = 0;
        len = read_u32(r);
    }

    out.assign(reinterpret_cast<const char*>(buf_.data() + r + sizeof(uint32_t)), len);
    r += sizeof(uint32_t) + len;

    read_pos_.store(r, std::memory_order_release);
    count_.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

bool MessageRingBuffer::empty() const {
    return read_pos_.load(std::memory_order_acquire) ==
           write_pos_.load(std::memory_order_acquire);
}

size_t MessageRingBuffer::count_items() const {
    return count_.load(std::memory_order_relaxed);
}

size_t MessageRingBuffer::capacity_bytes() const {
    return capacity_;
}

size_t MessageRingBuffer::used_bytes() const {
    const size_t w = write_pos_.load(std::memory_order_relaxed);
    const size_t r = read_pos_.load(std::memory_order_relaxed);
    if (w >= r) return w - r;
    return (capacity_ - r) + w;
}

void MessageRingBuffer::write_u32(const size_t pos, const uint32_t val) {
    std::memcpy(buf_.data() + pos, &val, sizeof(val));
}

uint32_t MessageRingBuffer::read_u32(const size_t pos) const {
    uint32_t val;
    std::memcpy(&val, buf_.data() + pos, sizeof(val));
    return val;
}