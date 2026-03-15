#include "tcp_server.h"

#include <ws2tcpip.h>
#include <mstcpip.h>
#include "protocol.h"
#include "log/logger.h"

// ============================================================
// Helpers
// ============================================================

std::string TCPServer::peer_addr(const SOCKET sock) {
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getpeername(sock, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
    }
    return "unknown";
}

void TCPServer::enable_keepalive(const SOCKET sock) const {
    struct tcp_keepalive ka{};
    ka.onoff             = 1;
    ka.keepalivetime     = static_cast<ULONG>(tcp_cfg_.keepalive_time_ms);
    ka.keepaliveinterval = static_cast<ULONG>(tcp_cfg_.keepalive_interval_ms);

    DWORD bytes_returned = 0;
    WSAIoctl(sock, SIO_KEEPALIVE_VALS,
             &ka, sizeof(ka),
             nullptr, 0, &bytes_returned,
             nullptr, nullptr);
}

// ============================================================
// Lifecycle
// ============================================================

TCPServer::~TCPServer() {
    stop();
}

bool TCPServer::start(const int port, CommandHandler cmd_handler, const TCPConfig& tcp_cfg) {
    cmd_handler_ = std::move(cmd_handler);
    port_ = port;
    tcp_cfg_ = tcp_cfg;
    bcast_ring_ = std::make_unique<MessageRingBuffer>(static_cast<size_t>(tcp_cfg.buffer_size_kb) * 1024);

    LOG_DEBUG("TCP", "Config: cmd_recv=%dms, stream_send=%dms, keepalive=%dms/%dms, handshake=%dms",
              tcp_cfg_.cmd_recv_timeout_ms, tcp_cfg_.stream_send_timeout_ms,
              tcp_cfg_.keepalive_time_ms, tcp_cfg_.keepalive_interval_ms,
              tcp_cfg_.handshake_timeout_ms);

    stop_event_ = WSACreateEvent();
    dll_response_event_ = WSACreateEvent();
    broadcast_event_ = WSACreateEvent();
    dll_cmd_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    if (stop_event_ == WSA_INVALID_EVENT ||
        dll_response_event_ == WSA_INVALID_EVENT ||
        broadcast_event_ == WSA_INVALID_EVENT ||
        dll_cmd_event_ == nullptr) {
        LOG_ERROR("TCP", "Failed to create events");
        return false;
    }

    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET) {
        LOG_ERROR("TCP", "socket() failed: %d", WSAGetLastError());
        return false;
    }

    constexpr int opt = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<u_short>(port));

    if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("TCP", "bind() failed: %d", WSAGetLastError());
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listen_sock_, SOMAXCONN) == SOCKET_ERROR) {
        LOG_ERROR("TCP", "listen() failed: %d", WSAGetLastError());
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        return false;
    }

    listen_event_ = WSACreateEvent();
    WSAEventSelect(listen_sock_, listen_event_, FD_ACCEPT);

    running_ = true;
    io_thread_ = std::thread(&TCPServer::io_loop, this);
    dll_thread_ = std::thread(&TCPServer::dll_loop, this);
    bcast_thread_ = std::thread(&TCPServer::broadcast_loop, this);

    LOG_INFO("TCP", "Listening on port %d (buffer size: %zuKB)",
         port, bcast_ring_->capacity_bytes() / 1024);
    return true;
}

void TCPServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    LOG_INFO("TCP", "Stopping server...");

    WSASetEvent(stop_event_);
    SetEvent(dll_cmd_event_);

    if (io_thread_.joinable()) io_thread_.join();
    if (dll_thread_.joinable()) dll_thread_.join();
    if (bcast_thread_.joinable()) bcast_thread_.join();

    if (listen_sock_ != INVALID_SOCKET) {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
    }

    for (int i = 0; i < cmd_client_count_; ++i) {
        closesocket(cmd_clients_[i].sock);
        WSACloseEvent(cmd_clients_[i].event);
    }
    cmd_client_count_ = 0;

    {
        std::lock_guard lock(streams_mutex_);
        for (const SOCKET s : stream_clients_) closesocket(s);
        stream_clients_.clear();
    }

    if (listen_event_ != WSA_INVALID_EVENT) { WSACloseEvent(listen_event_); listen_event_ = WSA_INVALID_EVENT; }
    if (stop_event_ != WSA_INVALID_EVENT) { WSACloseEvent(stop_event_); stop_event_ = WSA_INVALID_EVENT; }
    if (dll_response_event_ != WSA_INVALID_EVENT) { WSACloseEvent(dll_response_event_); dll_response_event_ = WSA_INVALID_EVENT; }
    if (broadcast_event_ != WSA_INVALID_EVENT) { WSACloseEvent(broadcast_event_); broadcast_event_ = WSA_INVALID_EVENT; }
    if (dll_cmd_event_ != nullptr) { CloseHandle(dll_cmd_event_); dll_cmd_event_ = nullptr; }
}
