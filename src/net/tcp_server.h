#pragma once

#include <winsock2.h>

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>

#include "ring_buffer.h"

struct TCPConfig {
    int cmd_recv_timeout_ms;
    int stream_send_timeout_ms;
    int keepalive_time_ms;
    int keepalive_interval_ms;
    int handshake_timeout_ms;
    int buffer_size_kb;
};

class TCPServer {
public:
    using CommandHandler = std::function<std::string(const std::string&)>;

    TCPServer() = default;
    ~TCPServer();

    TCPServer(const TCPServer&) = delete;
    TCPServer& operator=(const TCPServer&) = delete;

    bool start(int port, CommandHandler cmd_handler, const TCPConfig& tcp_cfg);
    void stop();
    void broadcast(const std::string& message);

private:
    // === IO thread ===
    void io_loop();
    void handle_accept();
    void handle_cmd_client(int idx);
    void handle_dll_responses();

    void send_pending_writes();

    void register_cmd_client(SOCKET sock);
    void remove_cmd_client(int idx);

    void enable_keepalive(SOCKET sock) const;
    static std::string peer_addr(SOCKET sock);

    void broadcast_loop();

    void dll_loop();

    // Non-blocking recv helper. Returns bytes read, 0 if would block, -1 on error/close.
    static int nb_recv(SOCKET sock, uint8_t* buf, int len);
    // Non-blocking send helper. Returns bytes sent, 0 if would block, -1 on error.
    static int nb_send(SOCKET sock, const uint8_t* buf, int len);

    // --- Per-client state for non-blocking frame reads ---
    enum class ReadState { HEADER, PAYLOAD, WAITING_DLL };

    struct CmdClient {
        SOCKET      sock = INVALID_SOCKET;
        WSAEVENT    event = WSA_INVALID_EVENT;
        std::string peer;
        ReadState   read_state = ReadState::HEADER;

        // Partial read buffers
        uint8_t     header_buf[4] = {};
        int         header_bytes = 0;
        std::vector<char> payload_buf;
        int         payload_bytes = 0;
        uint32_t    payload_len = 0;

        // Pending write (DLL response)
        std::vector<uint8_t> write_buf;
        int         write_offset = 0;
    };

    // --- DLL command/response passing ---
    struct DllRequest {
        SOCKET client_sock;
        std::string command;
    };

    struct DllResponse {
        SOCKET client_sock;
        std::string response;
    };

    // IO thread state
    TCPConfig tcp_cfg_{};
    CommandHandler cmd_handler_;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread io_thread_;
    std::thread dll_thread_;
    std::thread bcast_thread_;

    SOCKET listen_sock_ = INVALID_SOCKET;
    WSAEVENT listen_event_ = WSA_INVALID_EVENT;
    WSAEVENT stop_event_ = WSA_INVALID_EVENT;
    WSAEVENT dll_response_event_ = WSA_INVALID_EVENT;
    WSAEVENT broadcast_event_ = WSA_INVALID_EVENT;

    static constexpr int MAX_CMD_CLIENTS = 58; // 64 - 4 system events - 2 reserved
    CmdClient cmd_clients_[MAX_CMD_CLIENTS];
    int cmd_client_count_ = 0;

    // Stream clients (write-only, no events needed)
    std::mutex streams_mutex_;
    std::vector<SOCKET> stream_clients_;

    // Broadcast ring buffer
    std::unique_ptr<MessageRingBuffer> bcast_ring_;

    // IO thread → DLL thread
    std::mutex dll_queue_mutex_;
    std::queue<DllRequest> dll_queue_;
    HANDLE dll_cmd_event_ = nullptr;

    // DLL thread → IO thread
    std::mutex response_mutex_;
    std::queue<DllResponse> response_queue_;
};