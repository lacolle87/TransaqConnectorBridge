#include "config.h"

#include <fstream>
#include <sstream>
#include <cstdio>
#include <cerrno>
#include <cstdlib>

namespace {

std::string trim(const std::string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool parse_int(const std::string& s, int& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long val = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    out = static_cast<int>(val);
    return true;
}

using Setter = bool(*)(Config&, const std::string&);

struct KeyMapping {
    const char* section;
    const char* key;
    Setter setter;
};

    const KeyMapping mappings[] = {
        {"server",   "dll_path",              [](Config& c, const std::string& v) { c.dll_path = v; return true; }},
        {"server",   "port",                  [](Config& c, const std::string& v) { return parse_int(v, c.port); }},
        {"server", "buffer_size_kb",          [](Config& c, const std::string& v) { return parse_int(v, c.buffer_size_kb); }},
        {"logging",  "path",                  [](Config& c, const std::string& v) { c.log_path = v; return true; }},
        {"logging",  "dll_path",              [](Config& c, const std::string& v) { c.dll_log_path = v; return true; }},
        {"logging",  "level",                 [](Config& c, const std::string& v) { return parse_int(v, c.log_level); }},
        {"logging",  "max_size_mb",           [](Config& c, const std::string& v) { return parse_int(v, c.log_max_size_mb); }},
        {"logging",  "max_files",             [](Config& c, const std::string& v) { return parse_int(v, c.log_max_files); }},
        {"logging",  "tz_offset",             [](Config& c, const std::string& v) { return parse_int(v, c.log_tz_offset); }},
        {"timeouts", "cmd_recv_ms",           [](Config& c, const std::string& v) { return parse_int(v, c.cmd_recv_timeout_ms); }},
        {"timeouts", "stream_send_ms",        [](Config& c, const std::string& v) { return parse_int(v, c.stream_send_timeout_ms); }},
        {"timeouts", "keepalive_time_ms",     [](Config& c, const std::string& v) { return parse_int(v, c.keepalive_time_ms); }},
        {"timeouts", "keepalive_interval_ms", [](Config& c, const std::string& v) { return parse_int(v, c.keepalive_interval_ms); }},
        {"timeouts", "handshake_ms",          [](Config& c, const std::string& v) { return parse_int(v, c.handshake_timeout_ms); }},
    };

} // namespace

namespace config {

bool load(const std::string& path, Config& out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        fprintf(stderr, "[CONFIG] Cannot open config file: %s\n", path.c_str());
        return false;
    }

    out = Config{};
    std::string current_section;
    std::string line;
    int line_num = 0;

    while (std::getline(file, line)) {
        ++line_num;
        line = trim(line);

        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            current_section = trim(line.substr(1, line.size() - 2));
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            fprintf(stderr, "[CONFIG] Line %d: missing '=': %s\n", line_num, line.c_str());
            return false;
        }

        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));

        bool found = false;
        for (const auto& [section, key_name, setter] : mappings) {
            if (current_section == section && key == key_name) {
                if (!setter(out, val)) {
                    fprintf(stderr, "[CONFIG] Line %d: invalid value for [%s] %s = %s\n",
                            line_num, current_section.c_str(), key.c_str(), val.c_str());
                    return false;
                }
                found = true;
                break;
            }
        }

        if (!found) {
            fprintf(stderr, "[CONFIG] Line %d: unknown key [%s] %s\n",
                    line_num, current_section.c_str(), key.c_str());
            return false;
        }
    }

    return true;
}

bool validate(const Config& cfg) {
    bool ok = true;

    auto check_range = [&](const char* name, const int val, const int lo, const int hi) {
        if (val < lo || val > hi) {
            fprintf(stderr, "[CONFIG] %s = %d out of range [%d, %d]\n", name, val, lo, hi);
            ok = false;
        }
    };

    check_range("port",                  cfg.port,                     1,    65535);
    check_range("buffer_size_kb",        cfg.buffer_size_kb,          64,    16384);
    check_range("log_level",             cfg.log_level,                1,    3);
    check_range("log_max_size_mb",       cfg.log_max_size_mb,          1,    1024);
    check_range("log_max_files",         cfg.log_max_files,            1,    100);
    check_range("log_tz_offset",         cfg.log_tz_offset,           -12,   14);
    check_range("cmd_recv_ms",           cfg.cmd_recv_timeout_ms,      100,  60000);
    check_range("stream_send_ms",        cfg.stream_send_timeout_ms,   1000, 120000);
    check_range("keepalive_time_ms",     cfg.keepalive_time_ms,        1000, 600000);
    check_range("keepalive_interval_ms", cfg.keepalive_interval_ms,    1000, 60000);
    check_range("handshake_ms",          cfg.handshake_timeout_ms,     1000, 60000);

    return ok;
}

} // namespace config
