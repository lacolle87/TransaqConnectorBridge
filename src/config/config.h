#pragma once

#include <string>

struct Config {
    // [server]
    std::string dll_path;
    int         port;
    int         buffer_size_kb;

    // [logging]
    std::string log_path;
    std::string dll_log_path;
    int         log_level;
    int         log_max_size_mb;
    int         log_max_files;
    int         log_tz_offset;

    // [timeouts] (milliseconds)
    int         cmd_recv_timeout_ms;
    int         stream_send_timeout_ms;
    int         keepalive_time_ms;
    int         keepalive_interval_ms;
    int         handshake_timeout_ms;
};

namespace config {

    bool load(const std::string& path, Config& out);
    bool validate(const Config& cfg);

} // namespace config
