#include <gtest/gtest.h>
#include "config.h"

#include <windows.h>
#include <fstream>
#include <string>

namespace {

// Full valid config for tests that need a valid starting point
const char* FULL_INI =
    "[server]\n"
    "dll_path = txmlconnector64.dll\n"
    "port = 9090\n"
    "buffer_size_kb = 512\n"
    "\n"
    "[logging]\n"
    "path = .\\logs\n"
    "dll_path = .\\dll_logs\n"
    "level = 2\n"
    "max_size_mb = 10\n"
    "max_files = 5\n"
    "tz_offset = 3\n"
    "\n"
    "[timeouts]\n"
    "cmd_recv_ms = 1000\n"
    "stream_send_ms = 10000\n"
    "keepalive_time_ms = 60000\n"
    "keepalive_interval_ms = 10000\n"
    "handshake_ms = 10000\n";

class ConfigTest : public ::testing::Test {
protected:
    std::string test_dir_;
    int file_counter_ = 0;

    void SetUp() override {
        static int dir_counter = 0;
        test_dir_ = "test_config_" + std::to_string(GetCurrentProcessId())
                   + "_" + std::to_string(dir_counter++);
        CreateDirectoryA(test_dir_.c_str(), nullptr);
    }

    void TearDown() override {
        WIN32_FIND_DATAA fd;
        std::string pattern = test_dir_ + "\\*";
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    DeleteFileA((test_dir_ + "\\" + fd.cFileName).c_str());
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
        RemoveDirectoryA(test_dir_.c_str());
    }

    std::string write_ini(const std::string& content) {
        std::string path = test_dir_ + "\\test_" + std::to_string(file_counter_++) + ".ini";
        std::ofstream f(path);
        f << content;
        f.close();
        return path;
    }

    // Load a full valid config for mutation in validation tests
    Config load_valid() {
        std::string path = write_ini(FULL_INI);
        Config cfg;
        config::load(path, cfg);
        return cfg;
    }
};

// ============================================================
// Loading
// ============================================================

TEST_F(ConfigTest, LoadFullConfig) {
    std::string path = write_ini(
        "[server]\n"
        "dll_path = my.dll\n"
        "port = 8080\n"
        "\n"
        "[logging]\n"
        "path = .\\my_logs\n"
        "dll_path = .\\my_dll_logs\n"
        "level = 3\n"
        "max_size_mb = 20\n"
        "max_files = 10\n"
        "tz_offset = -5\n"
        "\n"
        "[timeouts]\n"
        "cmd_recv_ms = 2000\n"
        "stream_send_ms = 15000\n"
        "keepalive_time_ms = 30000\n"
        "keepalive_interval_ms = 5000\n"
        "handshake_ms = 3000\n"
    );

    Config cfg;
    ASSERT_TRUE(config::load(path, cfg));

    EXPECT_EQ(cfg.dll_path, "my.dll");
    EXPECT_EQ(cfg.port, 8080);
    EXPECT_EQ(cfg.log_path, ".\\my_logs");
    EXPECT_EQ(cfg.dll_log_path, ".\\my_dll_logs");
    EXPECT_EQ(cfg.log_level, 3);
    EXPECT_EQ(cfg.log_max_size_mb, 20);
    EXPECT_EQ(cfg.log_max_files, 10);
    EXPECT_EQ(cfg.log_tz_offset, -5);
    EXPECT_EQ(cfg.cmd_recv_timeout_ms, 2000);
    EXPECT_EQ(cfg.stream_send_timeout_ms, 15000);
    EXPECT_EQ(cfg.keepalive_time_ms, 30000);
    EXPECT_EQ(cfg.keepalive_interval_ms, 5000);
    EXPECT_EQ(cfg.handshake_timeout_ms, 3000);
}

TEST_F(ConfigTest, PartialConfigZeroInitUnset) {
    std::string path = write_ini(
        "[server]\n"
        "port = 7070\n"
    );

    Config cfg;
    ASSERT_TRUE(config::load(path, cfg));

    EXPECT_EQ(cfg.port, 7070);
    // Unset fields are zero-initialized
    EXPECT_TRUE(cfg.dll_path.empty());
    EXPECT_EQ(cfg.log_level, 0);
    EXPECT_EQ(cfg.cmd_recv_timeout_ms, 0);
}

TEST_F(ConfigTest, CommentsAndBlankLines) {
    std::string path = write_ini(
        "# This is a comment\n"
        "; This is also a comment\n"
        "\n"
        "[server]\n"
        "port = 5555\n"
        "\n"
        "# Another comment\n"
    );

    Config cfg;
    ASSERT_TRUE(config::load(path, cfg));
    EXPECT_EQ(cfg.port, 5555);
}

TEST_F(ConfigTest, WhitespaceHandling) {
    std::string path = write_ini(
        "  [server]  \n"
        "  port   =   1234  \n"
        "  dll_path  =  path with spaces.dll  \n"
    );

    Config cfg;
    ASSERT_TRUE(config::load(path, cfg));
    EXPECT_EQ(cfg.port, 1234);
    EXPECT_EQ(cfg.dll_path, "path with spaces.dll");
}

TEST_F(ConfigTest, EmptyConfig) {
    std::string path = write_ini("");

    Config cfg;
    ASSERT_TRUE(config::load(path, cfg));

    // All fields zero-initialized
    EXPECT_TRUE(cfg.dll_path.empty());
    EXPECT_EQ(cfg.port, 0);
    EXPECT_TRUE(cfg.log_path.empty());
    EXPECT_TRUE(cfg.dll_log_path.empty());
    EXPECT_EQ(cfg.log_level, 0);
    EXPECT_EQ(cfg.log_max_size_mb, 0);
    EXPECT_EQ(cfg.log_max_files, 0);
    EXPECT_EQ(cfg.log_tz_offset, 0);
    EXPECT_EQ(cfg.cmd_recv_timeout_ms, 0);
    EXPECT_EQ(cfg.stream_send_timeout_ms, 0);
    EXPECT_EQ(cfg.keepalive_time_ms, 0);
    EXPECT_EQ(cfg.keepalive_interval_ms, 0);
    EXPECT_EQ(cfg.handshake_timeout_ms, 0);
}

// ============================================================
// Error cases
// ============================================================

TEST_F(ConfigTest, FileNotFound) {
    Config cfg;
    EXPECT_FALSE(config::load("nonexistent_file.ini", cfg));
}

TEST_F(ConfigTest, InvalidInteger) {
    std::string path = write_ini(
        "[server]\n"
        "port = abc\n"
    );

    Config cfg;
    EXPECT_FALSE(config::load(path, cfg));
}

TEST_F(ConfigTest, MissingEquals) {
    std::string path = write_ini(
        "[server]\n"
        "port 9090\n"
    );

    Config cfg;
    EXPECT_FALSE(config::load(path, cfg));
}

TEST_F(ConfigTest, UnknownKey) {
    std::string path = write_ini(
        "[server]\n"
        "unknown_key = 42\n"
    );

    Config cfg;
    EXPECT_FALSE(config::load(path, cfg));
}

TEST_F(ConfigTest, UnknownSection) {
    std::string path = write_ini(
        "[unknown]\n"
        "port = 9090\n"
    );

    Config cfg;
    EXPECT_FALSE(config::load(path, cfg));
}

TEST_F(ConfigTest, FloatAsInteger) {
    std::string path = write_ini(
        "[server]\n"
        "port = 9090.5\n"
    );

    Config cfg;
    EXPECT_FALSE(config::load(path, cfg));
}

TEST_F(ConfigTest, NegativePort) {
    std::string path = write_ini(
        "[server]\n"
        "port = -1\n"
    );

    Config cfg;
    ASSERT_TRUE(config::load(path, cfg));
    EXPECT_FALSE(config::validate(cfg));
}

// ============================================================
// Validation
// ============================================================

TEST_F(ConfigTest, ValidFullConfig) {
    Config cfg = load_valid();
    EXPECT_TRUE(config::validate(cfg));
}

TEST_F(ConfigTest, InvalidPort) {
    Config cfg = load_valid();

    cfg.port = 0;
    EXPECT_FALSE(config::validate(cfg));

    cfg.port = 70000;
    EXPECT_FALSE(config::validate(cfg));
}

TEST_F(ConfigTest, InvalidLogLevel) {
    Config cfg = load_valid();

    cfg.log_level = 0;
    EXPECT_FALSE(config::validate(cfg));

    cfg.log_level = 4;
    EXPECT_FALSE(config::validate(cfg));
}

TEST_F(ConfigTest, InvalidTimeouts) {
    Config cfg = load_valid();
    cfg.cmd_recv_timeout_ms = 10;
    EXPECT_FALSE(config::validate(cfg));

    cfg = load_valid();
    cfg.keepalive_time_ms = 999999;
    EXPECT_FALSE(config::validate(cfg));
}

TEST_F(ConfigTest, InvalidTzOffset) {
    Config cfg = load_valid();

    cfg.log_tz_offset = -13;
    EXPECT_FALSE(config::validate(cfg));

    cfg.log_tz_offset = 15;
    EXPECT_FALSE(config::validate(cfg));
}

TEST_F(ConfigTest, BoundaryValuesPass) {
    Config cfg = load_valid();

    cfg.port = 1;
    cfg.buffer_size_kb = 64;

    cfg.log_level = 1;
    cfg.log_tz_offset = -12;
    cfg.cmd_recv_timeout_ms = 100;
    cfg.stream_send_timeout_ms = 1000;
    cfg.keepalive_time_ms = 1000;
    cfg.keepalive_interval_ms = 1000;
    cfg.handshake_timeout_ms = 1000;
    EXPECT_TRUE(config::validate(cfg));

    cfg.port = 65535;
    cfg.buffer_size_kb = 16384;
    cfg.log_level = 3;
    cfg.log_tz_offset = 14;
    cfg.cmd_recv_timeout_ms = 60000;
    cfg.stream_send_timeout_ms = 120000;
    cfg.keepalive_time_ms = 600000;
    cfg.keepalive_interval_ms = 60000;
    cfg.handshake_timeout_ms = 60000;
    EXPECT_TRUE(config::validate(cfg));
}

} // namespace