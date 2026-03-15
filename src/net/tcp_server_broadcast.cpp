#include "tcp_server.h"

#include "protocol.h"
#include "log/logger.h"

#include <algorithm>

// ============================================================
// Broadcast — push to ring buffer, signal broadcast thread
// ============================================================

// ReSharper disable once CppMemberFunctionMayBeConst
void TCPServer::broadcast(const std::string& message) {
    if (!running_.load(std::memory_order_acquire)) return;

    if (!bcast_ring_->push(message)) {
        LOG_WARN("TCP", "Broadcast message too large (%zu bytes), dropped", message.size());
        return;
    }
    WSASetEvent(broadcast_event_);
}

static bool send_all_blocking(const SOCKET s, const uint8_t* data, const int len) {
    int sent = 0;
    while (sent < len) {
        const int n = send(s, reinterpret_cast<const char*>(data + sent), len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

void TCPServer::broadcast_loop() {
    LOG_DEBUG("TCP", "Broadcast thread started");

    const HANDLE wait_handles[2] = { stop_event_, broadcast_event_ };
    std::vector<uint8_t> frame_buf;
    frame_buf.reserve(4096);

    std::vector<SOCKET> clients_snapshot;
    std::vector<SOCKET> dead_clients;

    while (running_) {
        const DWORD result = WaitForMultipleObjects(2, wait_handles, FALSE, 1000);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_TIMEOUT) continue;

        WSAResetEvent(broadcast_event_);

        std::string message;
        while (bcast_ring_->pop(message)) {
            protocol::encode_frame(message, frame_buf);

            {
                std::lock_guard lock(streams_mutex_);
                clients_snapshot = stream_clients_;
            }

            dead_clients.clear();
            for (const SOCKET s : clients_snapshot) {
                if (!send_all_blocking(s, frame_buf.data(), static_cast<int>(frame_buf.size()))) {
                    LOG_WARN("TCP", "Stream client %s send failed/timeout, disconnecting",
                             peer_addr(s).c_str());
                    dead_clients.push_back(s);
                }
            }

            if (!dead_clients.empty()) {
                std::lock_guard lock(streams_mutex_);
                for (const SOCKET d : dead_clients) {
                    closesocket(d);
                    if (const auto it = std::find(stream_clients_.begin(), stream_clients_.end(), d);
                        it != stream_clients_.end()) {
                        *it = stream_clients_.back();
                        stream_clients_.pop_back();
                    }
                }
            }
        }
    }

    LOG_DEBUG("TCP", "Broadcast thread stopped");
}

// ============================================================
// DLL Thread — single thread for all DLL commands
// ============================================================

void TCPServer::dll_loop() {
    LOG_DEBUG("TCP", "DLL thread started");

    while (running_) {
        WaitForSingleObject(dll_cmd_event_, 1000);
        if (!running_) break;

        while (true) {
            DllRequest req;
            {
                std::lock_guard lock(dll_queue_mutex_);
                if (dll_queue_.empty()) break;
                req = std::move(dll_queue_.front());
                dll_queue_.pop();
            }
            {
                std::string response = cmd_handler_(req.command);
                std::lock_guard lock(response_mutex_);
                response_queue_.push(DllResponse{req.client_sock, std::move(response)});
            }
            WSASetEvent(dll_response_event_);
        }
    }

    LOG_DEBUG("TCP", "DLL thread stopped");
}