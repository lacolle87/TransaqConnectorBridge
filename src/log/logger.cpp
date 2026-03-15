#include "logger.h"

#include <io.h>
#include <cstring>
#include <ctime>
#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    shutdown();
}

bool Logger::init(const std::string& log_dir, const LogLevel level,
                  const size_t max_file_size, const int max_files,
                  const int utc_offset_hours) {
    std::lock_guard lock(mutex_);

    if (initialized_.load(std::memory_order_relaxed)) return true;

    log_dir_       = log_dir;
    level_.store(level, std::memory_order_relaxed);
    max_file_size_ = max_file_size;
    max_files_     = std::max(1, max_files);
    utc_offset_    = utc_offset_hours;

    CreateDirectoryA(log_dir_.c_str(), nullptr);

    open_file();
    if (!file_) {
        fprintf(stderr, "[LOGGER] Failed to open log file in: %s\n", log_dir_.c_str());
        return false;
    }

    initialized_.store(true, std::memory_order_release);
    return true;
}

void Logger::shutdown() {
    std::lock_guard lock(mutex_);

    initialized_.store(false, std::memory_order_release);

    if (file_) {
        fflush(file_);
        fclose(file_);
        file_ = nullptr;
    }
}

void Logger::set_level(const LogLevel level) {
    level_.store(level, std::memory_order_release);
}

void Logger::debug(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(LogLevel::DEBUG, tag, fmt, args);
    va_end(args);
}

void Logger::info(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(LogLevel::INFO, tag, fmt, args);
    va_end(args);
}

void Logger::warn(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(LogLevel::WARN, tag, fmt, args);
    va_end(args);
}

void Logger::error(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(LogLevel::ERR, tag, fmt, args);
    va_end(args);
}

bool Logger::sanitize(const char* input, char* out_buf, const size_t out_size) {
    static const char* sensitive_tags[] = {
        "password", "login", "token", "secret", "session",
    };

    if (!std::strchr(input, '<')) {
        return false;
    }

    bool has_sensitive = false;
    for (const char* tag : sensitive_tags) {
        if (std::strstr(input, tag)) {
            has_sensitive = true;
            break;
        }
    }

    if (!has_sensitive) {
        return false;
    }

    std::string result(input);

    for (const char* tag : sensitive_tags) {
        const std::string open = std::string("<") + tag + ">";
        const std::string close = std::string("</") + tag + ">";

        size_t pos = 0;
        while ((pos = result.find(open, pos)) != std::string::npos) {
            const size_t val_start = pos + open.size();
            const size_t val_end = result.find(close, val_start);
            if (val_end == std::string::npos) break;

            result.replace(val_start, val_end - val_start, "***");
            pos = val_start + 3 + close.size();
        }
    }

    snprintf(out_buf, out_size, "%s", result.c_str());
    return true;
}

// ReSharper disable once CppParameterMayBeConst
void Logger::vlog(const LogLevel level, const char* tag, const char* fmt, va_list args) {
    if (level > level_.load(std::memory_order_acquire)) return;

    char msg_buf[4096];
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);

    char sanitized_buf[4096];
    const char* safe_msg = sanitize(msg_buf, sanitized_buf, sizeof(sanitized_buf))
        ? sanitized_buf : msg_buf;

    const std::string ts = timestamp();
    const char* lvl = level_str(level);

    char line[4352];
    const int line_len = snprintf(line, sizeof(line), "%s [%s] [%s] %s\n",
                                  ts.c_str(), lvl, tag, safe_msg);

    std::lock_guard lock(mutex_);

    _write(_fileno(stderr), line, line_len);

    if (!file_) return;

    if (current_size_ + line_len > max_file_size_) {
        rotate();
    }

    if (file_) {
        fwrite(line, 1, line_len, file_);
        fflush(file_);
        current_size_ += line_len;
    }
}

void Logger::rotate() {
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }

    const std::string oldest = file_path(max_files_ - 1);
    DeleteFileA(oldest.c_str());

    for (int i = max_files_ - 2; i >= 0; --i) {
        const std::string src = file_path(i);
        const std::string dst = file_path(i + 1);
        MoveFileA(src.c_str(), dst.c_str());
    }

    open_file();
}

void Logger::open_file() {
    const std::string path = file_path(0);
    file_ = fopen(path.c_str(), "a");
    if (file_) {
        fseek(file_, 0, SEEK_END);
        current_size_ = static_cast<size_t>(ftell(file_));
    } else {
        current_size_ = 0;
    }
}

std::string Logger::file_path(const int index) const {
    if (index == 0) {
        return log_dir_ + "\\" + base_name_;
    }
    return log_dir_ + "\\" + base_name_ + "." + std::to_string(index);
}

const char* Logger::level_str(const LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERR:   return "ERROR";
        default:              return "?????";
    }
}

std::string Logger::timestamp() const {
    time_t now = time(nullptr);
    now += utc_offset_ * 3600;

    tm tm_buf{};
    gmtime_s(&tm_buf, &now);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buf;
}