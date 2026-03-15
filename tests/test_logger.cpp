#include <gtest/gtest.h>
#include "logger.h"

#include <windows.h>
#include <string>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace {

// Unique test directory per test to avoid conflicts
class LoggerTest : public ::testing::Test {
protected:
    std::string test_dir_;

    void SetUp() override {
        static int counter = 0;
        test_dir_ = "test_logs_" + std::to_string(GetCurrentProcessId())
                   + "_" + std::to_string(counter++);
        CreateDirectoryA(test_dir_.c_str(), nullptr);
    }

    void TearDown() override {
        Logger::instance().shutdown();

        // Clean up test files
        WIN32_FIND_DATAA fd;
        std::string pattern = test_dir_ + "\\*";
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::string path = test_dir_ + "\\" + fd.cFileName;
                    DeleteFileA(path.c_str());
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
        RemoveDirectoryA(test_dir_.c_str());
    }

    std::string log_file_path(int index = 0) const {
        if (index == 0) return test_dir_ + "\\tcbridge.log";
        return test_dir_ + "\\tcbridge.log." + std::to_string(index);
    }

    std::string read_file(const std::string& path) const {
        std::ifstream f(path);
        if (!f.is_open()) return "";
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    bool file_exists(const std::string& path) const {
        DWORD attr = GetFileAttributesA(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES;
    }

    int count_lines(const std::string& content) const {
        if (content.empty()) return 0;
        int n = 0;
        for (char c : content) {
            if (c == '\n') ++n;
        }
        return n;
    }
};

// ============================================================
// Initialization
// ============================================================

TEST_F(LoggerTest, InitSuccess) {
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG));
    EXPECT_TRUE(Logger::instance().initialized());
    EXPECT_TRUE(file_exists(log_file_path()));
}

TEST_F(LoggerTest, InitIdempotent) {
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG));
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG));
    EXPECT_TRUE(Logger::instance().initialized());
}

TEST_F(LoggerTest, ShutdownAndReinit) {
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG));
    Logger::instance().shutdown();
    EXPECT_FALSE(Logger::instance().initialized());

    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG));
    EXPECT_TRUE(Logger::instance().initialized());
}

// ============================================================
// Log levels
// ============================================================

TEST_F(LoggerTest, DebugLevelLogsAll) {
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG));

    LOG_DEBUG("TEST", "debug msg");
    LOG_INFO("TEST", "info msg");
    LOG_WARN("TEST", "warn msg");
    LOG_ERROR("TEST", "error msg");

    Logger::instance().shutdown();

    std::string content = read_file(log_file_path());
    EXPECT_NE(content.find("debug msg"), std::string::npos);
    EXPECT_NE(content.find("info msg"), std::string::npos);
    EXPECT_NE(content.find("warn msg"), std::string::npos);
    EXPECT_NE(content.find("error msg"), std::string::npos);
}

TEST_F(LoggerTest, InfoLevelFiltersDebug) {
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::INFO));

    LOG_DEBUG("TEST", "should not appear");
    LOG_INFO("TEST", "should appear");

    Logger::instance().shutdown();

    std::string content = read_file(log_file_path());
    EXPECT_EQ(content.find("should not appear"), std::string::npos);
    EXPECT_NE(content.find("should appear"), std::string::npos);
}

TEST_F(LoggerTest, ErrorLevelFiltersLower) {
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::ERR));

    LOG_DEBUG("TEST", "no");
    LOG_INFO("TEST", "no");
    LOG_WARN("TEST", "no");
    LOG_ERROR("TEST", "yes");

    Logger::instance().shutdown();

    std::string content = read_file(log_file_path());
    EXPECT_EQ(count_lines(content), 1);
    EXPECT_NE(content.find("yes"), std::string::npos);
}

TEST_F(LoggerTest, SetLevelRuntime) {
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::ERR));

    LOG_INFO("TEST", "hidden");

    Logger::instance().set_level(LogLevel::DEBUG);
    LOG_INFO("TEST", "visible");

    Logger::instance().shutdown();

    std::string content = read_file(log_file_path());
    EXPECT_EQ(content.find("hidden"), std::string::npos);
    EXPECT_NE(content.find("visible"), std::string::npos);
}

// ============================================================
// Formatting
// ============================================================

TEST_F(LoggerTest, FormatContainsTagAndLevel) {
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG));

    LOG_INFO("MYTAG", "hello %s %d", "world", 42);

    Logger::instance().shutdown();

    std::string content = read_file(log_file_path());
    EXPECT_NE(content.find("[INFO ]"), std::string::npos);
    EXPECT_NE(content.find("[MYTAG]"), std::string::npos);
    EXPECT_NE(content.find("hello world 42"), std::string::npos);
}

TEST_F(LoggerTest, FormatContainsTimestamp) {
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG));

    LOG_INFO("TEST", "timestamp test");

    Logger::instance().shutdown();

    std::string content = read_file(log_file_path());
    // Should contain date pattern like "2025-" or "2026-"
    EXPECT_TRUE(content.find("202") != std::string::npos);
}

// ============================================================
// File rotation
// ============================================================

TEST_F(LoggerTest, RotationCreatesBackupFiles) {
    // Small max size to trigger rotation quickly
    const size_t max_size = 200;
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG, max_size, 3));

    // Write enough to trigger at least one rotation
    for (int i = 0; i < 50; ++i) {
        LOG_INFO("TEST", "rotation test message number %d padding", i);
    }

    Logger::instance().shutdown();

    // Should have rotated files
    EXPECT_TRUE(file_exists(log_file_path(0)));
    EXPECT_TRUE(file_exists(log_file_path(1)));
}

TEST_F(LoggerTest, RotationRespectsMaxFiles) {
    const size_t max_size = 150;
    const int max_files = 3;
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG, max_size, max_files));

    // Write a lot to trigger many rotations
    for (int i = 0; i < 200; ++i) {
        LOG_INFO("TEST", "message %d with padding for size", i);
    }

    Logger::instance().shutdown();

    // Should have at most max_files files (indices 0 to max_files-1)
    EXPECT_TRUE(file_exists(log_file_path(0)));
    // File at index max_files should NOT exist (deleted by rotation)
    EXPECT_FALSE(file_exists(log_file_path(max_files)));
}

// ============================================================
// Thread safety
// ============================================================

TEST_F(LoggerTest, ConcurrentWrites) {
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG));

    constexpr int NUM_THREADS = 4;
    constexpr int MSGS_PER_THREAD = 100;

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([t, MSGS_PER_THREAD]() {
            const std::string tag = "T" + std::to_string(t);
            for (int i = 0; i < MSGS_PER_THREAD; ++i) {
                Logger::instance().info(tag.c_str(), "msg %d", i);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    Logger::instance().shutdown();

    std::string content = read_file(log_file_path());
    int lines = count_lines(content);
    EXPECT_EQ(lines, NUM_THREADS * MSGS_PER_THREAD);
}

// ============================================================
// Edge cases
// ============================================================

TEST_F(LoggerTest, LogBeforeInit) {
    // Should not crash
    LOG_INFO("TEST", "before init");
    SUCCEED();
}

TEST_F(LoggerTest, LogAfterShutdown) {
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG));
    Logger::instance().shutdown();

    // Should not crash
    LOG_INFO("TEST", "after shutdown");
    SUCCEED();
}

TEST_F(LoggerTest, EmptyMessage) {
    ASSERT_TRUE(Logger::instance().init(test_dir_, LogLevel::DEBUG));
    LOG_INFO("TEST", "");
    Logger::instance().shutdown();

    std::string content = read_file(log_file_path());
    EXPECT_FALSE(content.empty());
}

} // namespace
