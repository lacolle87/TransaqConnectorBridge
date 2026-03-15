#include "tcp_server.h"

#include <ws2tcpip.h>
#include "protocol.h"
#include "log/logger.h"

// Event array indices (must match tcp_server.cpp)
static constexpr int EVT_STOP      = 0;
static constexpr int EVT_LISTEN    = 1;
static constexpr int EVT_DLL_RESP  = 2;
static constexpr int EVT_CLIENTS   = 3;

// ============================================================
// IO Thread — single thread handles ALL socket I/O
// ============================================================

void TCPServer::io_loop() {
    LOG_DEBUG("TCP", "IO thread started");

    while (running_) {
        WSAEVENT events[WSA_MAXIMUM_WAIT_EVENTS];
        events[EVT_STOP]      = stop_event_;
        events[EVT_LISTEN]    = listen_event_;
        events[EVT_DLL_RESP]  = dll_response_event_;
        for (int i = 0; i < cmd_client_count_; ++i) {
            events[EVT_CLIENTS + i] = cmd_clients_[i].event;
        }
        const int total_events = EVT_CLIENTS + cmd_client_count_;

        const DWORD result = WSAWaitForMultipleEvents(
            total_events, events, FALSE, 1000, FALSE);

        if (result == WSA_WAIT_FAILED) {
            LOG_ERROR("TCP", "WSAWaitForMultipleEvents failed: %d", WSAGetLastError());
            break;
        }

        if (result == WSA_WAIT_TIMEOUT) continue;
        if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) break;
        if (WaitForSingleObject(listen_event_, 0) == WAIT_OBJECT_0) {
            handle_accept();
        }
        if (WaitForSingleObject(dll_response_event_, 0) == WAIT_OBJECT_0) {
            WSAResetEvent(dll_response_event_);
            handle_dll_responses();
        }

        for (int i = cmd_client_count_ - 1; i >= 0; --i) {
            if (WaitForSingleObject(cmd_clients_[i].event, 0) == WAIT_OBJECT_0) {
                handle_cmd_client(i);
            }
        }

        send_pending_writes();
    }

    LOG_DEBUG("TCP", "IO thread stopped");
}

// ============================================================
// Accept — read channel byte, route to cmd or stream
// ============================================================

void TCPServer::handle_accept() {
    WSANETWORKEVENTS net_events;
    WSAEnumNetworkEvents(listen_sock_, listen_event_, &net_events);

    if (!(net_events.lNetworkEvents & FD_ACCEPT)) return;

    const SOCKET client = accept(listen_sock_, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
        LOG_WARN("TCP", "accept() failed: %d", WSAGetLastError());
        return;
    }

    const std::string peer = peer_addr(client);
    enable_keepalive(client);

    const auto timeout = static_cast<DWORD>(tcp_cfg_.handshake_timeout_ms);
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    uint8_t channel_type = 0;
    if (recv(client, reinterpret_cast<char*>(&channel_type), 1, 0) != 1) {
        LOG_WARN("TCP", "Handshake failed from %s (err=%d)", peer.c_str(), WSAGetLastError());
        closesocket(client);
        return;
    }

    switch (channel_type) {
        case protocol::CHANNEL_COMMAND:
            if (cmd_client_count_ >= MAX_CMD_CLIENTS) {
                LOG_WARN("TCP", "Max command clients reached, rejecting %s", peer.c_str());
                closesocket(client);
                return;
            }
            LOG_INFO("TCP", "Command client connected: %s", peer.c_str());
            register_cmd_client(client);
            break;

        case protocol::CHANNEL_STREAM: {
            LOG_INFO("TCP", "Stream client connected: %s", peer.c_str());
            const auto tv = static_cast<DWORD>(tcp_cfg_.stream_send_timeout_ms);
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&tv), sizeof(tv));
            std::lock_guard lock(streams_mutex_);
            stream_clients_.push_back(client);
            break;
        }

        default:
            LOG_WARN("TCP", "Unknown channel type 0x%02x from %s", channel_type, peer.c_str());
            closesocket(client);
            break;
    }
}

void TCPServer::register_cmd_client(const SOCKET sock) {
    CmdClient& c = cmd_clients_[cmd_client_count_];
    c = CmdClient{};
    c.sock = sock;
    c.peer = peer_addr(sock);
    c.event = WSACreateEvent();
    c.payload_buf.reserve(4096);

    WSAEventSelect(sock, c.event, FD_READ | FD_CLOSE);
    ++cmd_client_count_;
}

void TCPServer::remove_cmd_client(const int idx) {
    LOG_INFO("TCP", "[%s] Command client disconnected", cmd_clients_[idx].peer.c_str());

    closesocket(cmd_clients_[idx].sock);
    WSACloseEvent(cmd_clients_[idx].event);

    if (idx < cmd_client_count_ - 1) {
        cmd_clients_[idx] = std::move(cmd_clients_[cmd_client_count_ - 1]);
    }
    --cmd_client_count_;
}

// ============================================================
// Command client — non-blocking frame read state machine
// ============================================================

void TCPServer::handle_cmd_client(const int idx) {
    // ReSharper disable once CppUseStructuredBinding
    CmdClient& c = cmd_clients_[idx];

    WSANETWORKEVENTS net_events;
    WSAEnumNetworkEvents(c.sock, c.event, &net_events);

    if (net_events.lNetworkEvents & FD_CLOSE) {
        remove_cmd_client(idx);
        return;
    }

    if (!(net_events.lNetworkEvents & FD_READ)) return;
    if (c.read_state == ReadState::WAITING_DLL) return;

    while (true) {
        if (c.read_state == ReadState::HEADER) {
            const int need = 4 - c.header_bytes;
            const int got = nb_recv(c.sock, c.header_buf + c.header_bytes, need);
            if (got < 0) { remove_cmd_client(idx); return; }
            if (got == 0) break;
            c.header_bytes += got;

            if (c.header_bytes < 4) continue;

            c.payload_len = protocol::decode_length(c.header_buf);
            c.header_bytes = 0;

            if (c.payload_len == 0 || c.payload_len > protocol::MAX_FRAME_SIZE) {
                LOG_WARN("TCP", "[%s] Invalid frame length: %u", c.peer.c_str(), c.payload_len);
                remove_cmd_client(idx);
                return;
            }

            c.payload_buf.resize(c.payload_len);
            c.payload_bytes = 0;
            c.read_state = ReadState::PAYLOAD;
        }

        if (c.read_state == ReadState::PAYLOAD) {
            const int need = static_cast<int>(c.payload_len) - c.payload_bytes;
            const int got = nb_recv(c.sock, reinterpret_cast<uint8_t*>(c.payload_buf.data()) + c.payload_bytes, need);
            if (got < 0) { remove_cmd_client(idx); return; }
            if (got == 0) break;
            c.payload_bytes += got;

            if (c.payload_bytes < static_cast<int>(c.payload_len)) continue;

            std::string command(c.payload_buf.data(), c.payload_len);
            LOG_DEBUG("TCP", "[%s] Received command (%u bytes)", c.peer.c_str(), c.payload_len);

            c.read_state = ReadState::WAITING_DLL;

            {
                std::lock_guard lock(dll_queue_mutex_);
                dll_queue_.push(DllRequest{c.sock, std::move(command)});
            }
            SetEvent(dll_cmd_event_);
            break;
        }
    }
}

int TCPServer::nb_recv(const SOCKET sock, uint8_t* buf, const int len) {
    const int n = recv(sock, reinterpret_cast<char*>(buf), len, 0);
    if (n > 0) return n;
    if (n == 0) return -1;
    if (const int err = WSAGetLastError(); err == WSAEWOULDBLOCK) return 0;
    return -1;
}

int TCPServer::nb_send(const SOCKET sock, const uint8_t* buf, const int len) {
    const int n = send(sock, reinterpret_cast<const char*>(buf), len, 0);
    if (n > 0) return n;
    if (n == 0) return 0;
    if (const int err = WSAGetLastError(); err == WSAEWOULDBLOCK) return 0;
    return -1;
}

// ============================================================
// DLL responses → send back to command clients
// ============================================================

void TCPServer::handle_dll_responses() {
    std::queue<DllResponse> responses;
    {
        std::lock_guard lock(response_mutex_);
        responses.swap(response_queue_);
    }

    while (!responses.empty()) {
        auto [client_sock, response] = std::move(responses.front());
        responses.pop();

        int idx = -1;
        for (int i = 0; i < cmd_client_count_; ++i) {
            if (cmd_clients_[i].sock == client_sock) {
                idx = i;
                break;
            }
        }
        if (idx < 0) continue;

        // ReSharper disable once CppUseStructuredBinding
        CmdClient& c = cmd_clients_[idx];

        LOG_DEBUG("TCP", "[%s] Sending DLL response (%zu bytes)", c.peer.c_str(), response.size());

        protocol::encode_frame(response, c.write_buf);
        c.write_offset = 0;

        const int total = static_cast<int>(c.write_buf.size());
        const int sent = nb_send(c.sock, c.write_buf.data(), total);
        if (sent < 0) {
            remove_cmd_client(idx);
            continue;
        }
        c.write_offset = sent;

        if (sent >= total) {
            c.write_buf.clear();
            c.write_offset = 0;
            c.read_state = ReadState::HEADER;
        }
    }
}

// ============================================================
// Flush pending writes on command clients
// ============================================================

void TCPServer::send_pending_writes() {
    for (int i = 0; i < cmd_client_count_; ++i) {
        // ReSharper disable once CppUseStructuredBinding
        CmdClient& c = cmd_clients_[i];
        if (c.write_buf.empty() || c.write_offset >= static_cast<int>(c.write_buf.size())) continue;

        const int remaining = static_cast<int>(c.write_buf.size()) - c.write_offset;
        const int sent = nb_send(c.sock, c.write_buf.data() + c.write_offset, remaining);
        if (sent < 0) {
            remove_cmd_client(i);
            --i;
            continue;
        }
        c.write_offset += sent;

        if (c.write_offset >= static_cast<int>(c.write_buf.size())) {
            c.write_buf.clear();
            c.write_offset = 0;
            c.read_state = ReadState::HEADER;
        }
    }
}
