#include <gtest/gtest.h>
#include "ring_buffer.h"

#include <string>
#include <vector>
#include <thread>
#include <atomic>

// ============================================================
// Basic push/pop
// ============================================================

TEST(RingBuffer, PushPopSingle) {
    MessageRingBuffer rb(1024);

    std::string out;
    EXPECT_FALSE(rb.pop(out));
    EXPECT_TRUE(rb.empty());

    EXPECT_TRUE(rb.push("hello"));
    EXPECT_FALSE(rb.empty());

    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, "hello");
    EXPECT_TRUE(rb.empty());
}

TEST(RingBuffer, PushPopMultiple) {
    MessageRingBuffer rb(4096);

    EXPECT_TRUE(rb.push("aaa"));
    EXPECT_TRUE(rb.push("bbb"));
    EXPECT_TRUE(rb.push("ccc"));

    std::string out;
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, "aaa");
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, "bbb");
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, "ccc");
    EXPECT_FALSE(rb.pop(out));
}

TEST(RingBuffer, FifoOrder) {
    MessageRingBuffer rb(8192);

    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(rb.push("msg_" + std::to_string(i)));
    }

    std::string out;
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(rb.pop(out));
        EXPECT_EQ(out, "msg_" + std::to_string(i));
    }
    EXPECT_FALSE(rb.pop(out));
}

// ============================================================
// Empty buffer
// ============================================================

TEST(RingBuffer, EmptyOnConstruction) {
    MessageRingBuffer rb(1024);
    EXPECT_TRUE(rb.empty());

    std::string out;
    EXPECT_FALSE(rb.pop(out));
}

TEST(RingBuffer, EmptyAfterDrain) {
    MessageRingBuffer rb(1024);

    rb.push("x");
    std::string out;
    rb.pop(out);

    EXPECT_TRUE(rb.empty());
    EXPECT_FALSE(rb.pop(out));
}

// ============================================================
// Empty string
// ============================================================

TEST(RingBuffer, EmptyString) {
    MessageRingBuffer rb(1024);

    EXPECT_TRUE(rb.push(""));
    std::string out = "not_empty";
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, "");
}

// ============================================================
// Wrap-around
// ============================================================

TEST(RingBuffer, WrapAround) {
    // Small buffer: 128 bytes. Each entry = 4 (len) + payload.
    // With 50-byte messages, entry = 54 bytes.
    // Push-then-pop to advance write_pos, forcing a wrap.
    MessageRingBuffer rb(128);

    std::string msg50(50, 'A');
    EXPECT_TRUE(rb.push(msg50));
    std::string out;
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, msg50);

    // write_pos is now at 54. Push another — fits before end.
    EXPECT_TRUE(rb.push(msg50));
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, msg50);

    // write_pos is now at 108. Next push won't fit at end — must wrap.
    // read_pos is also at 108 (empty), so after sentinel write_pos wraps to 0.
    EXPECT_TRUE(rb.push(msg50));
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, msg50);
}

TEST(RingBuffer, WrapAroundPreservesData) {
    MessageRingBuffer rb(256);

    // Fill and drain several times to force wrapping
    for (int round = 0; round < 10; ++round) {
        std::string msg = "round_" + std::to_string(round) + "_payload";
        EXPECT_TRUE(rb.push(msg));

        std::string out;
        EXPECT_TRUE(rb.pop(out));
        EXPECT_EQ(out, msg);
    }
}

TEST(RingBuffer, WrapWithMultipleEntries) {
    // Buffer 256 bytes. Push several entries, pop some, push more to trigger wrap.
    MessageRingBuffer rb(256);

    // Fill: 5 entries of ~20 bytes each (entry = 4 + 20 = 24 bytes, total 120)
    std::string msg20(20, 'X');
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(rb.push(msg20));
    }

    // Drain 3 entries — frees 72 bytes at start
    std::string out;
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(rb.pop(out));
    }

    // Push 3 more — should trigger wrap
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(rb.push(msg20));
    }

    // Drain all remaining (2 old + 3 new = 5)
    int count = 0;
    while (rb.pop(out)) {
        EXPECT_EQ(out, msg20);
        ++count;
    }
    EXPECT_EQ(count, 5);
}

// ============================================================
// Buffer full — push returns false (no eviction)
// ============================================================

TEST(RingBuffer, FullBufferReturnsFalse) {
    // Buffer 64 bytes. Entry "aa" = 4 + 2 = 6 bytes.
    // Plus room for sentinel (4 bytes). Effective ~56 bytes = ~9 entries.
    MessageRingBuffer rb(64);

    int pushed = 0;
    for (int i = 0; i < 50; ++i) {
        if (rb.push("aa")) {
            ++pushed;
        } else {
            break;
        }
    }

    EXPECT_GT(pushed, 0);
    EXPECT_LT(pushed, 50);

    // All pushed messages should be recoverable in order
    std::string out;
    for (int i = 0; i < pushed; ++i) {
        EXPECT_TRUE(rb.pop(out));
        EXPECT_EQ(out, "aa");
    }
    EXPECT_FALSE(rb.pop(out));
}

TEST(RingBuffer, PushAfterFullAndDrain) {
    MessageRingBuffer rb(64);

    // Fill
    while (rb.push("xx")) {}

    // Drain
    std::string out;
    while (rb.pop(out)) {}

    // Should work again
    EXPECT_TRUE(rb.push("yy"));
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, "yy");
}

// ============================================================
// Message too large for buffer
// ============================================================

TEST(RingBuffer, RejectsTooLargeMessage) {
    MessageRingBuffer rb(64);

    std::string huge(100, 'Z');
    EXPECT_FALSE(rb.push(huge));

    // Buffer should still work after rejected push
    EXPECT_TRUE(rb.push("ok"));
    std::string out;
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, "ok");
}

TEST(RingBuffer, MessageExactlyMaxCapacity) {
    // Entry = 4 (len) + payload. Need room for sentinel (4) after entry.
    // So max payload = capacity - 8.
    constexpr size_t cap = 256;
    MessageRingBuffer rb(cap);

    // This should fail: payload + header + sentinel > capacity
    std::string big(cap - 4, 'X');
    EXPECT_FALSE(rb.push(big));

    // This should fit: payload + header + sentinel = capacity
    std::string fits(cap - 8, 'Y');
    EXPECT_TRUE(rb.push(fits));

    std::string out;
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, fits);
}

// ============================================================
// Binary data / null bytes
// ============================================================

TEST(RingBuffer, BinaryPayload) {
    MessageRingBuffer rb(1024);

    std::string binary;
    binary.push_back('\x00');
    binary.push_back('\xFF');
    binary.push_back('\x42');
    binary.push_back('\x00');

    EXPECT_TRUE(rb.push(binary));

    std::string out;
    EXPECT_TRUE(rb.pop(out));
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], '\x00');
    EXPECT_EQ(out[1], '\xFF');
    EXPECT_EQ(out[2], '\x42');
    EXPECT_EQ(out[3], '\x00');
}

// ============================================================
// UTF-8 / XML payloads (like real TRANSAQ messages)
// ============================================================

TEST(RingBuffer, Utf8Payload) {
    MessageRingBuffer rb(4096);

    std::string utf8 = u8"<server_status connected=\"true\" recover=\"false\"/>";
    EXPECT_TRUE(rb.push(utf8));

    std::string out;
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, utf8);
}

TEST(RingBuffer, XmlMessages) {
    MessageRingBuffer rb(8192);

    std::vector<std::string> messages = {
        R"(<server_status connected="true"/>)",
        R"(<trades><trade secid="1" price="100.5"/></trades>)",
        R"(<orders><order transactionid="123" status="active"/></orders>)",
        R"(<quotations><quotation secid="1"><bid>99.5</bid><offer>100.5</offer></quotation></quotations>)",
    };

    for (const auto& msg : messages) {
        EXPECT_TRUE(rb.push(msg));
    }

    std::string out;
    for (const auto& msg : messages) {
        EXPECT_TRUE(rb.pop(out));
        EXPECT_EQ(out, msg);
    }
}

// ============================================================
// push(const char*, uint32_t) overload
// ============================================================

TEST(RingBuffer, PushCharPtr) {
    MessageRingBuffer rb(1024);

    const char* data = "raw_data";
    EXPECT_TRUE(rb.push(data, 8));

    std::string out;
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, "raw_data");
}

TEST(RingBuffer, PushCharPtrZeroLength) {
    MessageRingBuffer rb(1024);

    EXPECT_TRUE(rb.push("anything", 0));

    std::string out;
    EXPECT_TRUE(rb.pop(out));
    EXPECT_EQ(out, "");
}

// ============================================================
// Metrics (count_items, capacity_bytes, used_bytes)
// ============================================================

TEST(RingBuffer, CountItems) {
    MessageRingBuffer rb(4096);

    EXPECT_EQ(rb.count_items(), 0u);

    rb.push("a");
    EXPECT_EQ(rb.count_items(), 1u);

    rb.push("b");
    rb.push("c");
    EXPECT_EQ(rb.count_items(), 3u);

    std::string out;
    rb.pop(out);
    EXPECT_EQ(rb.count_items(), 2u);

    rb.pop(out);
    rb.pop(out);
    EXPECT_EQ(rb.count_items(), 0u);
}

TEST(RingBuffer, CapacityBytes) {
    MessageRingBuffer rb(2048);
    EXPECT_EQ(rb.capacity_bytes(), 2048u);
}

TEST(RingBuffer, UsedBytesGrowsAndShrinks) {
    MessageRingBuffer rb(4096);

    EXPECT_EQ(rb.used_bytes(), 0u);

    rb.push("hello");  // 4 + 5 = 9 bytes
    EXPECT_GT(rb.used_bytes(), 0u);

    size_t used_after_one = rb.used_bytes();

    rb.push("world");  // 4 + 5 = 9 more
    EXPECT_GT(rb.used_bytes(), used_after_one);

    std::string out;
    rb.pop(out);
    EXPECT_LT(rb.used_bytes(), used_after_one * 2);
}

// ============================================================
// Stress: many push/pop cycles (detect corruption)
// ============================================================

TEST(RingBuffer, StressPushPop) {
    MessageRingBuffer rb(4096);

    for (int i = 0; i < 10000; ++i) {
        std::string msg = "stress_" + std::to_string(i);
        EXPECT_TRUE(rb.push(msg));

        std::string out;
        EXPECT_TRUE(rb.pop(out));
        EXPECT_EQ(out, msg);
    }
    EXPECT_TRUE(rb.empty());
}

TEST(RingBuffer, StressBurstAndDrain) {
    MessageRingBuffer rb(8192);

    for (int round = 0; round < 100; ++round) {
        // Burst: push until full or 50 messages
        int pushed = 0;
        for (int i = 0; i < 50; ++i) {
            if (rb.push("r" + std::to_string(round) + "_m" + std::to_string(i))) {
                ++pushed;
            }
        }

        // Drain all
        std::string out;
        int count = 0;
        while (rb.pop(out)) {
            ++count;
        }
        EXPECT_EQ(count, pushed);
    }
}

// ============================================================
// Varying message sizes
// ============================================================

TEST(RingBuffer, MixedSizes) {
    MessageRingBuffer rb(4096);

    std::vector<std::string> messages;
    messages.emplace_back(1, 'A');       // tiny
    messages.emplace_back(100, 'B');     // medium
    messages.emplace_back(1000, 'C');    // large
    messages.emplace_back(10, 'D');      // small
    messages.emplace_back(500, 'E');     // medium-large

    for (const auto& msg : messages) {
        EXPECT_TRUE(rb.push(msg));
    }

    std::string out;
    for (const auto& msg : messages) {
        EXPECT_TRUE(rb.pop(out));
        EXPECT_EQ(out, msg);
    }
}

// ============================================================
// Thread safety: SPSC producer + consumer
// ============================================================

TEST(RingBuffer, ConcurrentProducerConsumer) {
    MessageRingBuffer rb(64 * 1024);
    constexpr int NUM_MESSAGES = 10000;
    std::atomic<int> consumed{0};
    std::atomic<bool> producer_done{false};

    // Single producer
    std::thread producer([&]() {
        for (int i = 0; i < NUM_MESSAGES; ++i) {
            std::string msg = std::to_string(i);
            while (!rb.push(msg)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Single consumer
    std::thread consumer([&]() {
        std::string out;
        int expected = 0;
        while (expected < NUM_MESSAGES) {
            if (rb.pop(out)) {
                // Verify FIFO ordering
                EXPECT_EQ(out, std::to_string(expected));
                ++expected;
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consumed.load(), NUM_MESSAGES);
    EXPECT_TRUE(rb.empty());
}

TEST(RingBuffer, ConcurrentSmallBuffer) {
    // Small buffer forces frequent wrapping under contention.
    MessageRingBuffer rb(512);
    constexpr int NUM_MESSAGES = 5000;
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        for (int i = 0; i < NUM_MESSAGES; ++i) {
            std::string msg = "m" + std::to_string(i);
            while (!rb.push(msg)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    int consumed = 0;
    std::thread consumer([&]() {
        std::string out;
        int expected = 0;
        while (expected < NUM_MESSAGES) {
            if (rb.pop(out)) {
                EXPECT_EQ(out, "m" + std::to_string(expected));
                ++expected;
                ++consumed;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consumed, NUM_MESSAGES);
    EXPECT_TRUE(rb.empty());
}

TEST(RingBuffer, ConcurrentLargeMessages) {
    // Stress wrap-around with large messages.
    MessageRingBuffer rb(16 * 1024);
    constexpr int NUM_MESSAGES = 500;
    std::string payload(1000, 'Z');

    std::thread producer([&]() {
        for (int i = 0; i < NUM_MESSAGES; ++i) {
            while (!rb.push(payload)) {
                std::this_thread::yield();
            }
        }
    });

    int consumed = 0;
    std::thread consumer([&]() {
        std::string out;
        for (int i = 0; i < NUM_MESSAGES; ++i) {
            while (!rb.pop(out)) {
                std::this_thread::yield();
            }
            EXPECT_EQ(out, payload);
            ++consumed;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consumed, NUM_MESSAGES);
}