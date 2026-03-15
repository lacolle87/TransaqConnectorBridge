#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

class DLLWrapper {
public:
    using MessageCallback = std::function<void(std::string)>;

    DLLWrapper() = default;
    ~DLLWrapper();

    DLLWrapper(const DLLWrapper&) = delete;
    DLLWrapper& operator=(const DLLWrapper&) = delete;

    bool load(const std::string& dll_path);
    bool initialize(const std::string& log_path, int log_level);
    bool set_callback(MessageCallback cb);
    std::string send_command(const std::string& command);
    void uninitialize();
    void unload();

    [[nodiscard]] bool is_connected() const {
        return dll_connected_.load(std::memory_order_acquire);
    }

private:
    void uninitialize_locked();

    using InitializeFn    = BYTE* (__stdcall*)(const BYTE* logPath, int logLevel);
    using UnInitializeFn  = BYTE* (__stdcall*)();
    using SetCallbackExFn = bool  (__stdcall*)(bool (__stdcall*)(BYTE* pData, void*), void*);
    using SendCommandFn   = BYTE* (__stdcall*)(BYTE* pData);
    using FreeMemoryFn    = bool  (__stdcall*)(BYTE* pData);

    static bool __stdcall static_callback(BYTE* pData, void* userData);
    static bool is_passthrough_command(const std::string& command);

    HMODULE module_ = nullptr;
    bool initialized_ = false;

    InitializeFn    fn_initialize_      = nullptr;
    UnInitializeFn  fn_uninitialize_    = nullptr;
    SetCallbackExFn fn_set_callback_ex_ = nullptr;
    SendCommandFn   fn_send_command_    = nullptr;
    FreeMemoryFn    fn_free_memory_     = nullptr;

    MessageCallback callback_;
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> dll_connected_{false};
    std::atomic<int>  active_callbacks_{0};

    std::mutex api_mutex_;
    std::mutex cb_mutex_;
    std::vector<char> cmd_buf_;
};