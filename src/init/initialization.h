#pragma once

#include <string>
#include <atomic>
#include "dll_wrapper.h"
#include "tcp_server.h"

namespace initialization {

    bool load_dll(DLLWrapper& dll, const std::string& dll_path);
    bool initialize_dll(DLLWrapper& dll, const std::string& log_path, int log_level);
    bool set_dll_callback(DLLWrapper& dll, TCPServer& server);
    bool start_tcp_server(TCPServer& server, DLLWrapper& dll, int tcp_port, const TCPConfig& tcp_cfg);
    void wait_for_shutdown(const std::atomic<bool>& running);
    void cleanup(TCPServer& server, DLLWrapper& dll);

} // namespace initialization
