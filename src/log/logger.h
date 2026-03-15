#pragma once

#include <string>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstdarg>

enum class LogLevel : int {
    ERR   = 0,
    WARN  = 1,
    INFO  = 2,
    DEBUG = 3
};

class Logger {
public:
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static Logger& instance();

    [[nodiscard]] bool init(const std::string& log_dir,
              LogLevel level         = LogLevel::INFO,
              size_t max_file_size   = 10 * 1024 * 1024,
              int max_files          = 5,
              int utc_offset_hours   = 0);

    void shutdown();
    void set_level(LogLevel level);

    void debug(const char* tag, const char* fmt, ...);
    void info (const char* tag, const char* fmt, ...);
    void warn (const char* tag, const char* fmt, ...);
    void error(const char* tag, const char* fmt, ...);

    [[nodiscard]] bool        initialized() const { return initialized_.load(std::memory_order_acquire); }
    [[nodiscard]] size_t      current_size() const { return current_size_; }
    [[nodiscard]] std::string log_dir()     const { return log_dir_; }

private:
    Logger() = default;
    ~Logger();

    void vlog(LogLevel level, const char* tag, const char* fmt, va_list args);
    void rotate();
    void open_file();

    [[nodiscard]] std::string file_path(int index) const;
    [[nodiscard]] static const char* level_str(LogLevel level);
    [[nodiscard]] std::string timestamp() const;

    static bool sanitize(const char* input, char* out_buf, size_t out_size);

    std::mutex  mutex_;
    FILE*       file_          = nullptr;
    std::string log_dir_;
    std::string base_name_     = "tcbridge.log";
    std::atomic<LogLevel> level_{LogLevel::INFO};
    size_t      max_file_size_ = 10 * 1024 * 1024;
    int         max_files_     = 5;
    int         utc_offset_    = 0;
    size_t      current_size_  = 0;
    std::atomic<bool> initialized_{false};
};

#define LOG_DEBUG(tag, fmt, ...) Logger::instance().debug(tag, fmt, ##__VA_ARGS__)
#define LOG_INFO(tag, fmt, ...)  Logger::instance().info(tag, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)  Logger::instance().warn(tag, fmt, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...) Logger::instance().error(tag, fmt, ##__VA_ARGS__)