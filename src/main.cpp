#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <atomic>

#include "config.h"
#include "dll_wrapper.h"
#include "tcp_server.h"
#include "initialization.h"
#include "logger.h"

static std::atomic g_running{true};

static void signal_handler(const int sig) {
    (void)sig;
    g_running = false;
}

static std::string get_config_path() {
    const char* val = std::getenv("TCBRIDGE_CONFIG");
    return val ? std::string(val) : std::string("config.ini");
}

struct WinsockGuard {
    bool ok = false;

    WinsockGuard() {
        WSADATA wsa;
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        if (!ok) LOG_ERROR("MAIN", "WSAStartup failed");
    }

    ~WinsockGuard() {
        if (ok) WSACleanup();
    }

    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;
};

int main() {
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    const std::string config_path = get_config_path();

    Config cfg;
    if (!config::load(config_path, cfg)) {
        fprintf(stderr, "[MAIN] Failed to load config from: %s\n", config_path.c_str());
        return 1;
    }

    if (!config::validate(cfg)) {
        fprintf(stderr, "[MAIN] Config validation failed\n");
        return 1;
    }

    if (const auto app_log_level = static_cast<LogLevel>(cfg.log_level);
        !Logger::instance().init(cfg.log_path, app_log_level,
        static_cast<size_t>(cfg.log_max_size_mb) * 1024 * 1024,
        cfg.log_max_files,
        cfg.log_tz_offset)) {
        fprintf(stderr, "[MAIN] Failed to initialize logger\n");
        return 1;
        }

    // ReSharper disable once CppTooWideScopeInitStatement
    const WinsockGuard wsa;
    if (!wsa.ok) return 1;

    LOG_INFO("MAIN", "Transaq Connector Bridge starting");
    LOG_INFO("MAIN", "Config: %s", config_path.c_str());
    LOG_INFO("MAIN", "DLL: %s, port: %d, log_level: %d",
             cfg.dll_path.c_str(), cfg.port, cfg.log_level);

    TCPServer  server;
    DLLWrapper dll;

    if (!initialization::load_dll(dll, cfg.dll_path)) return 1;
    if (!initialization::initialize_dll(dll, cfg.dll_log_path, cfg.log_level)) return 1;
    if (!initialization::set_dll_callback(dll, server)) return 1;

    if (const TCPConfig tcp_cfg{
        cfg.cmd_recv_timeout_ms,
        cfg.stream_send_timeout_ms,
        cfg.keepalive_time_ms,
        cfg.keepalive_interval_ms,
        cfg.handshake_timeout_ms,
        cfg.buffer_size_kb,
    }; !initialization::start_tcp_server(server, dll, cfg.port, tcp_cfg)) return 1;

    LOG_INFO("MAIN", "Service ready on port %d", cfg.port);

    SetProcessWorkingSetSize(GetCurrentProcess(),
        static_cast<SIZE_T>(-1),
        static_cast<SIZE_T>(-1));

    initialization::wait_for_shutdown(g_running);
    LOG_INFO("MAIN", "Shutdown signal received");
    initialization::cleanup(server, dll);
    Logger::instance().shutdown();

    return 0;
}
