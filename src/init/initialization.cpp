#include <windows.h>
#include <atomic>
#include "initialization.h"
#include "logger.h"

namespace initialization {

    bool load_dll(DLLWrapper& dll, const std::string& dll_path) {
        if (!dll.load(dll_path)) {
            LOG_ERROR("MAIN", "Failed to load DLL: %s", dll_path.c_str());
            return false;
        }
        return true;
    }

    bool initialize_dll(DLLWrapper& dll, const std::string& log_path, const int log_level) {
        if (!dll.initialize(log_path, log_level)) {
            LOG_ERROR("MAIN", "Failed to initialize DLL");
            return false;
        }
        return true;
    }

    bool set_dll_callback(DLLWrapper& dll, TCPServer& server) {
        if (!dll.set_callback([&server](const std::string& msg) {
        server.broadcast(msg);
        })) {
            LOG_ERROR("MAIN", "Failed to set DLL callback");
            return false;
        }
        return true;
    }

    bool start_tcp_server(TCPServer& server, DLLWrapper& dll, const int tcp_port, const TCPConfig& tcp_cfg) {
        if (!server.start(tcp_port, [&dll](const std::string& cmd) -> std::string {
            return dll.send_command(cmd);
        }, tcp_cfg)) {
            LOG_ERROR("MAIN", "Failed to start TCP server on port %d", tcp_port);
            return false;
        }
        return true;
    }

    void wait_for_shutdown(const std::atomic<bool>& running) {
        while (running) {
            Sleep(500);
        }
    }

    void cleanup(TCPServer& server, DLLWrapper& dll) {
        LOG_INFO("MAIN", "Shutting down...");
        server.stop();
        dll.unload();
        LOG_INFO("MAIN", "Stopped");
    }

} // namespace initialization