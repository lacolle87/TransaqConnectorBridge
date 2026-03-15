#include <gtest/gtest.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "protocol.h"
#include "tcp_server.h"

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>

// ============================================================
// Global Winsock init (once for all tests)
// ============================================================

class WinsockEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        WSADATA wsa;
        ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsa), 0)
            << "WSAStartup failed";
    }
    void TearDown() override {
        WSACleanup();
    }
};

[[maybe_unused]] static auto* g_wsa_env =
    ::testing::AddGlobalTestEnvironment(new WinsockEnvironment);

// ============================================================
// Helpers
// ============================================================

namespace {

constexpr int TEST_PORT_BASE = 19000;
static std::atomic<int> g_port_counter{0};

int next_port() {
    return TEST_PORT_BASE + g_port_counter.fetch_add(1);
}

SOCKET connect_to(int port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    return sock;
}

bool send_all(SOCKET sock, const void* data, int len) {
    auto* buf = static_cast<const char*>(data);
    int sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool recv_all(SOCKET sock, void* data, int len) {
    auto* buf = static_cast<char*>(data);
    int received = 0;
    while (received < len) {
        int n = recv(sock, buf + received, len - received, 0);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

bool send_channel(SOCKET sock, uint8_t channel) {
    return send_all(sock, &channel, 1);
}

bool send_frame(SOCKET sock, const std::string& payload) {
    auto frame = protocol::encode_frame(payload);
    return send_all(sock, frame.data(), static_cast<int>(frame.size()));
}

std::string recv_frame(SOCKET sock, int timeout_ms = 3000) {
    DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));

    uint8_t len_buf[4];
    if (!recv_all(sock, len_buf, 4)) return "";

    uint32_t payload_len = protocol::decode_length(len_buf);
    if (payload_len == 0 || payload_len > protocol::MAX_FRAME_SIZE) return "";

    std::vector<char> buf(payload_len);
    if (!recv_all(sock, buf.data(), static_cast<int>(payload_len))) return "";

    return std::string(buf.data(), payload_len);
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

// ============================================================
// Mock command handler: records calls, returns canned response
// ============================================================

class MockHandler {
public:
    void set_response(const std::string& resp) {
        std::lock_guard<std::mutex> lock(mu_);
        response_ = resp;
    }

    std::string operator()(const std::string& cmd) {
        std::lock_guard<std::mutex> lock(mu_);
        received_commands_.push_back(cmd);
        return response_;
    }

    std::vector<std::string> received() const {
        std::lock_guard<std::mutex> lock(mu_);
        return received_commands_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        received_commands_.clear();
    }

private:
    mutable std::mutex mu_;
    std::string response_ = R"(<result success="true"/>)";
    std::vector<std::string> received_commands_;
};

// ============================================================
// Test fixture
// ============================================================

class TCPServerTest : public ::testing::Test {
protected:
    TCPServer server_;
    MockHandler handler_;
    int port_ = 0;
    TCPConfig tcp_cfg_{1000, 10000, 60000, 10000, 10000, 512};

    void SetUp() override {
        port_ = next_port();
    }

    void TearDown() override {
        server_.stop();
    }

    bool start_server() {
        bool ok = server_.start(port_, [this](const std::string& cmd) {
            return handler_(cmd);
        }, tcp_cfg_);
        if (ok) sleep_ms(50);
        return ok;
    }
};

// ============================================================
// Server lifecycle
// ============================================================

TEST_F(TCPServerTest, StartStop) {
    ASSERT_TRUE(start_server());
    SOCKET s = connect_to(port_);
    ASSERT_NE(s, INVALID_SOCKET);
    closesocket(s);
}

TEST_F(TCPServerTest, StopIdempotent) {
    ASSERT_TRUE(start_server());
    server_.stop();
    server_.stop();
}

// ============================================================
// Command channel
// ============================================================

TEST_F(TCPServerTest, CommandRequestResponse) {
    ASSERT_TRUE(start_server());

    handler_.set_response(R"(<result success="true"/>)");

    SOCKET cmd = connect_to(port_);
    ASSERT_NE(cmd, INVALID_SOCKET);

    ASSERT_TRUE(send_channel(cmd, protocol::CHANNEL_COMMAND));
    sleep_ms(30);

    std::string xml_cmd = R"(<command id="connect"><login>test</login></command>)";
    ASSERT_TRUE(send_frame(cmd, xml_cmd));

    std::string response = recv_frame(cmd);
    EXPECT_EQ(response, R"(<result success="true"/>)");

    auto cmds = handler_.received();
    ASSERT_EQ(cmds.size(), 1u);
    EXPECT_EQ(cmds[0], xml_cmd);

    closesocket(cmd);
}

TEST_F(TCPServerTest, MultipleCommandsSequential) {
    ASSERT_TRUE(start_server());

    SOCKET cmd = connect_to(port_);
    ASSERT_NE(cmd, INVALID_SOCKET);
    ASSERT_TRUE(send_channel(cmd, protocol::CHANNEL_COMMAND));
    sleep_ms(30);

    for (int i = 0; i < 5; ++i) {
        std::string xml = "<command id=\"" + std::to_string(i) + "\"/>";
        std::string expected_resp = "<response id=\"" + std::to_string(i) + "\"/>";
        handler_.set_response(expected_resp);

        ASSERT_TRUE(send_frame(cmd, xml));
        std::string resp = recv_frame(cmd);
        EXPECT_EQ(resp, expected_resp);
    }

    EXPECT_EQ(handler_.received().size(), 5u);
    closesocket(cmd);
}

TEST_F(TCPServerTest, MultipleCommandClients) {
    ASSERT_TRUE(start_server());

    handler_.set_response("<ok/>");

    constexpr int NUM_CLIENTS = 3;
    SOCKET clients[NUM_CLIENTS];

    for (int i = 0; i < NUM_CLIENTS; ++i) {
        clients[i] = connect_to(port_);
        ASSERT_NE(clients[i], INVALID_SOCKET);
        ASSERT_TRUE(send_channel(clients[i], protocol::CHANNEL_COMMAND));
    }
    sleep_ms(50);

    for (int i = 0; i < NUM_CLIENTS; ++i) {
        std::string cmd = "<cmd client=\"" + std::to_string(i) + "\"/>";
        ASSERT_TRUE(send_frame(clients[i], cmd));
        std::string resp = recv_frame(clients[i]);
        EXPECT_EQ(resp, "<ok/>");
    }

    EXPECT_EQ(handler_.received().size(), static_cast<size_t>(NUM_CLIENTS));

    for (int i = 0; i < NUM_CLIENTS; ++i) {
        closesocket(clients[i]);
    }
}

TEST_F(TCPServerTest, CommandLargePayload) {
    ASSERT_TRUE(start_server());

    std::string big_cmd(1024 * 1024, 'X');
    std::string big_resp(1024 * 1024, 'Y');
    handler_.set_response(big_resp);

    SOCKET cmd = connect_to(port_);
    ASSERT_NE(cmd, INVALID_SOCKET);
    ASSERT_TRUE(send_channel(cmd, protocol::CHANNEL_COMMAND));
    sleep_ms(30);

    ASSERT_TRUE(send_frame(cmd, big_cmd));
    std::string resp = recv_frame(cmd, 10000);
    EXPECT_EQ(resp, big_resp);

    closesocket(cmd);
}

// ============================================================
// Stream channel
// ============================================================

TEST_F(TCPServerTest, StreamReceivesBroadcast) {
    ASSERT_TRUE(start_server());

    SOCKET stream = connect_to(port_);
    ASSERT_NE(stream, INVALID_SOCKET);
    ASSERT_TRUE(send_channel(stream, protocol::CHANNEL_STREAM));
    sleep_ms(50);

    std::string msg = R"(<server_status connected="true" recover="false"/>)";
    server_.broadcast(msg);

    std::string received = recv_frame(stream);
    EXPECT_EQ(received, msg);

    closesocket(stream);
}

TEST_F(TCPServerTest, MultipleBroadcasts) {
    ASSERT_TRUE(start_server());

    SOCKET stream = connect_to(port_);
    ASSERT_NE(stream, INVALID_SOCKET);
    ASSERT_TRUE(send_channel(stream, protocol::CHANNEL_STREAM));
    sleep_ms(50);

    for (int i = 0; i < 10; ++i) {
        std::string msg = "<tick id=\"" + std::to_string(i) + "\"/>";
        server_.broadcast(msg);

        std::string received = recv_frame(stream);
        EXPECT_EQ(received, msg);
    }

    closesocket(stream);
}

TEST_F(TCPServerTest, BroadcastToMultipleStreams) {
    ASSERT_TRUE(start_server());

    constexpr int NUM_STREAMS = 3;
    SOCKET streams[NUM_STREAMS];

    for (int i = 0; i < NUM_STREAMS; ++i) {
        streams[i] = connect_to(port_);
        ASSERT_NE(streams[i], INVALID_SOCKET);
        ASSERT_TRUE(send_channel(streams[i], protocol::CHANNEL_STREAM));
    }
    sleep_ms(50);

    std::string msg = "<market_data/>";
    server_.broadcast(msg);

    for (int i = 0; i < NUM_STREAMS; ++i) {
        std::string received = recv_frame(streams[i]);
        EXPECT_EQ(received, msg);
    }

    for (int i = 0; i < NUM_STREAMS; ++i) {
        closesocket(streams[i]);
    }
}

TEST_F(TCPServerTest, BroadcastSkipsDisconnectedStreams) {
    ASSERT_TRUE(start_server());

    SOCKET s1 = connect_to(port_);
    SOCKET s2 = connect_to(port_);
    ASSERT_NE(s1, INVALID_SOCKET);
    ASSERT_NE(s2, INVALID_SOCKET);

    ASSERT_TRUE(send_channel(s1, protocol::CHANNEL_STREAM));
    ASSERT_TRUE(send_channel(s2, protocol::CHANNEL_STREAM));
    sleep_ms(50);

    closesocket(s1);
    sleep_ms(30);

    std::string msg = "<alive/>";
    server_.broadcast(msg);

    std::string received = recv_frame(s2);
    EXPECT_EQ(received, msg);

    closesocket(s2);
}

// ============================================================
// Mixed command + stream
// ============================================================

TEST_F(TCPServerTest, CommandAndStreamSimultaneous) {
    ASSERT_TRUE(start_server());

    handler_.set_response("<ack/>");

    SOCKET stream = connect_to(port_);
    ASSERT_NE(stream, INVALID_SOCKET);
    ASSERT_TRUE(send_channel(stream, protocol::CHANNEL_STREAM));
    sleep_ms(30);

    SOCKET cmd = connect_to(port_);
    ASSERT_NE(cmd, INVALID_SOCKET);
    ASSERT_TRUE(send_channel(cmd, protocol::CHANNEL_COMMAND));
    sleep_ms(30);

    ASSERT_TRUE(send_frame(cmd, "<test/>"));
    std::string cmd_resp = recv_frame(cmd);
    EXPECT_EQ(cmd_resp, "<ack/>");

    server_.broadcast("<event/>");
    std::string stream_msg = recv_frame(stream);
    EXPECT_EQ(stream_msg, "<event/>");

    closesocket(cmd);
    closesocket(stream);
}

// ============================================================
// Edge cases / error handling
// ============================================================

TEST_F(TCPServerTest, UnknownChannelTypeDisconnects) {
    ASSERT_TRUE(start_server());

    SOCKET s = connect_to(port_);
    ASSERT_NE(s, INVALID_SOCKET);

    uint8_t bad_channel = 0xFF;
    ASSERT_TRUE(send_all(s, &bad_channel, 1));
    sleep_ms(100);

    char buf[1];
    int n = recv(s, buf, 1, 0);
    EXPECT_LE(n, 0);

    closesocket(s);
}

TEST_F(TCPServerTest, CommandClientOversizedFrame) {
    ASSERT_TRUE(start_server());

    SOCKET cmd = connect_to(port_);
    ASSERT_NE(cmd, INVALID_SOCKET);
    ASSERT_TRUE(send_channel(cmd, protocol::CHANNEL_COMMAND));
    sleep_ms(30);

    uint32_t bad_len = protocol::MAX_FRAME_SIZE + 1;
    uint8_t len_buf[4] = {
        static_cast<uint8_t>((bad_len >> 24) & 0xFF),
        static_cast<uint8_t>((bad_len >> 16) & 0xFF),
        static_cast<uint8_t>((bad_len >> 8)  & 0xFF),
        static_cast<uint8_t>((bad_len)       & 0xFF),
    };
    ASSERT_TRUE(send_all(cmd, len_buf, 4));
    sleep_ms(100);

    char buf[1];
    int n = recv(cmd, buf, 1, 0);
    EXPECT_LE(n, 0);

    closesocket(cmd);
}

TEST_F(TCPServerTest, CommandClientZeroLengthFrame) {
    ASSERT_TRUE(start_server());

    SOCKET cmd = connect_to(port_);
    ASSERT_NE(cmd, INVALID_SOCKET);
    ASSERT_TRUE(send_channel(cmd, protocol::CHANNEL_COMMAND));
    sleep_ms(30);

    uint8_t len_buf[4] = {0, 0, 0, 0};
    ASSERT_TRUE(send_all(cmd, len_buf, 4));
    sleep_ms(100);

    char buf[1];
    int n = recv(cmd, buf, 1, 0);
    EXPECT_LE(n, 0);

    closesocket(cmd);
}

TEST_F(TCPServerTest, CommandClientDisconnectMidFrame) {
    ASSERT_TRUE(start_server());

    SOCKET cmd = connect_to(port_);
    ASSERT_NE(cmd, INVALID_SOCKET);
    ASSERT_TRUE(send_channel(cmd, protocol::CHANNEL_COMMAND));
    sleep_ms(30);

    uint8_t len_buf[4] = {0, 0, 0, 100};
    ASSERT_TRUE(send_all(cmd, len_buf, 4));
    closesocket(cmd);

    sleep_ms(100);

    SOCKET cmd2 = connect_to(port_);
    ASSERT_NE(cmd2, INVALID_SOCKET);
    ASSERT_TRUE(send_channel(cmd2, protocol::CHANNEL_COMMAND));
    sleep_ms(30);

    handler_.set_response("<ok/>");
    ASSERT_TRUE(send_frame(cmd2, "<ping/>"));
    std::string resp = recv_frame(cmd2);
    EXPECT_EQ(resp, "<ok/>");

    closesocket(cmd2);
}

// ============================================================
// Broadcast with no stream clients
// ============================================================

TEST_F(TCPServerTest, BroadcastWithNoClients) {
    ASSERT_TRUE(start_server());

    server_.broadcast("<test/>");
    server_.broadcast("<test2/>");
    sleep_ms(50);
}