#include "dll_wrapper.h"
#include "logger.h"

#include <vector>
#include <cstring>

static constexpr char NOT_CONNECTED_RESPONSE[] =
    "<result success=\"false\">"
    "<message>Not connected to server</message>"
    "</result>";

static std::string strip_xml(const std::string& xml) {
    const auto start = xml.find('>');
    if (const auto end = xml.rfind('<'); start != std::string::npos && end != std::string::npos && end > start) {
        return xml.substr(start + 1, end - start - 1);
    }
    return xml;
}

DLLWrapper::~DLLWrapper() {
    unload();
}

bool DLLWrapper::load(const std::string& dll_path) {
    std::lock_guard lock(api_mutex_);

    module_ = LoadLibraryA(dll_path.c_str());
    if (!module_) {
        LOG_ERROR("DLL", "LoadLibrary failed: %lu", GetLastError());
        return false;
    }

    fn_initialize_      = reinterpret_cast<InitializeFn>(GetProcAddress(module_, "Initialize"));
    fn_uninitialize_    = reinterpret_cast<UnInitializeFn>(GetProcAddress(module_, "UnInitialize"));
    fn_set_callback_ex_ = reinterpret_cast<SetCallbackExFn>(GetProcAddress(module_, "SetCallbackEx"));
    fn_send_command_    = reinterpret_cast<SendCommandFn>(GetProcAddress(module_, "SendCommand"));
    fn_free_memory_     = reinterpret_cast<FreeMemoryFn>(GetProcAddress(module_, "FreeMemory"));

    if (!fn_initialize_ || !fn_uninitialize_ || !fn_set_callback_ex_ ||
        !fn_send_command_ || !fn_free_memory_) {
        LOG_ERROR("DLL", "Failed to resolve one or more exported functions");
        FreeLibrary(module_);
        module_ = nullptr;
        return false;
    }

    LOG_INFO("DLL", "Loaded: %s", dll_path.c_str());
    return true;
}

bool DLLWrapper::initialize(const std::string& log_path, const int log_level) {
    std::lock_guard lock(api_mutex_);
    if (!module_) return false;

    if (BYTE* result = fn_initialize_(
            reinterpret_cast<const BYTE*>(log_path.c_str()),
            log_level
        )) {
        LOG_ERROR("DLL", "Initialize error: %s",
                  strip_xml(reinterpret_cast<const char*>(result)).c_str());
        fn_free_memory_(result);
        return false;
    }

    initialized_ = true;
    LOG_INFO("DLL", "Initialized (log_path=%s, log_level=%d)", log_path.c_str(), log_level);
    return true;
}

bool DLLWrapper::set_callback(MessageCallback cb) {
    std::lock_guard lock(api_mutex_);
    if (!module_) return false;

    {
        std::lock_guard cb_lock(cb_mutex_);
        callback_ = std::move(cb);
        shutting_down_.store(false, std::memory_order_release);
    }

    if (!fn_set_callback_ex_(static_callback, this)) {
        LOG_ERROR("DLL", "SetCallbackEx returned false");
        std::lock_guard cb_lock(cb_mutex_);
        callback_ = nullptr;
        return false;
    }

    LOG_INFO("DLL", "Callback set");
    return true;
}

bool DLLWrapper::is_passthrough_command(const std::string& command) {
    return command.find("id=\"connect\"") != std::string::npos ||
           command.find("id=\"disconnect\"") != std::string::npos ||
           command.find("id=\"server_status\"") != std::string::npos;
}

std::string DLLWrapper::send_command(const std::string& command) {
    std::lock_guard lock(api_mutex_);

    if (!module_ || !initialized_) {
        return "ERR:DLL not initialized";
    }

    if (!dll_connected_.load(std::memory_order_acquire) &&
        !is_passthrough_command(command)) {
        LOG_DEBUG("DLL", "Command rejected: not connected to server");
        return NOT_CONNECTED_RESPONSE;
    }

    cmd_buf_.assign(command.begin(), command.end());
    cmd_buf_.push_back('\0');

    if (cmd_buf_.capacity() > 64 * 1024 && command.size() < 4096) {
        cmd_buf_.shrink_to_fit();
    }

    if (BYTE* result = fn_send_command_(
            reinterpret_cast<BYTE*>(cmd_buf_.data())
        )) {
        std::string response(reinterpret_cast<const char*>(result));
        fn_free_memory_(result);

        if (response.empty()) {
            LOG_WARN("DLL", "Empty response from SendCommand");
            return "ERR:empty response from SendCommand";
        }

        return response;
    }

    LOG_WARN("DLL", "Null pointer from SendCommand");
    return "ERR:null pointer from SendCommand";
}

void DLLWrapper::uninitialize() {
    std::lock_guard lock(api_mutex_);
    uninitialize_locked();
}

void DLLWrapper::unload() {
    shutting_down_.store(true, std::memory_order_release);

    while (active_callbacks_.load(std::memory_order_acquire) > 0) {
        Sleep(1);
    }

    {
        std::lock_guard cb_lock(cb_mutex_);
        callback_ = nullptr;
    }

    std::lock_guard lock(api_mutex_);
    uninitialize_locked();

    while (active_callbacks_.load(std::memory_order_acquire) > 0) {
        Sleep(1);
    }

    if (module_) {
        FreeLibrary(module_);
        module_ = nullptr;
        LOG_INFO("DLL", "Unloaded");
    }

    dll_connected_.store(false, std::memory_order_release);

    std::vector<char>().swap(cmd_buf_);
}

void DLLWrapper::uninitialize_locked() {
    if (module_ && fn_uninitialize_ && initialized_) {
        if (BYTE* result = fn_uninitialize_()) {
            LOG_ERROR("DLL", "UnInitialize error: %s",
                      strip_xml(reinterpret_cast<const char*>(result)).c_str());
            fn_free_memory_(result);
        }
        initialized_ = false;
        dll_connected_.store(false, std::memory_order_release);
    }
}

bool __stdcall DLLWrapper::static_callback(BYTE* pData, void* userData) {
    if (!pData || !userData) return true;

    auto* self = static_cast<DLLWrapper*>(userData);

    self->active_callbacks_.fetch_add(1, std::memory_order_acq_rel);

    struct CallbackGuard {
        std::atomic<int>& counter;
        ~CallbackGuard() { counter.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{self->active_callbacks_};

    if (self->shutting_down_.load(std::memory_order_acquire)) {
        self->fn_free_memory_(pData);
        return true;
    }

    const auto* str = reinterpret_cast<const char*>(pData);
    const size_t len = std::strlen(str);
    std::string msg(str, len);

    self->fn_free_memory_(pData);

    if (const auto tag_start = msg.find("<server_status");
    tag_start != std::string::npos) {
        if (const auto tag_end = msg.find('>', tag_start);
            tag_end != std::string::npos) {
            const auto tag = std::string_view(msg).substr(tag_start, tag_end - tag_start + 1);
            const bool is_connected = tag.find("connected=\"true\"") != std::string_view::npos;
            self->dll_connected_.store(is_connected, std::memory_order_release);
            }
    }


    MessageCallback cb;
    {
        std::lock_guard lock(self->cb_mutex_);
        cb = self->callback_;
    }

    if (cb) {
        try {
            cb(std::move(msg));
        } catch (const std::exception& e) {
            LOG_ERROR("DLL", "Callback exception: %s", e.what());
            return false;
        }
    }

    return true;
}