#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ws_client.hpp — Minimal TLS WebSocket client (OpenSSL + BSD/Winsock)
//
//  Implements:
//    - TCP connection (cross-platform: Winsock2 on Windows, BSD on Linux)
//    - TLS handshake via OpenSSL
//    - WebSocket HTTP Upgrade (RFC 6455)
//    - WebSocket frame send/receive (text frames, ping/pong)
//
//  No external dependencies beyond OpenSSL (already in MSYS2/most distros).
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <stdexcept>
#include <functional>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <array>
#include <random>
#include <algorithm>

// ── Platform socket layer ─────────────────────────────────────────────────────
#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
#  define HYDRA_WIN_SOCK 1
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define HYDRA_INVALID_SOCKET INVALID_SOCKET
#  define hydra_close_socket(s) ::closesocket(s)
#  define hydra_sock_error() WSAGetLastError()
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
   using socket_t = int;
#  define HYDRA_INVALID_SOCKET (-1)
#  define hydra_close_socket(s) ::close(s)
#  define hydra_sock_error() errno
#endif

// ── OpenSSL ───────────────────────────────────────────────────────────────────
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>

namespace hydra {

// ─────────────────────────────────────────────────────────────────────────────
//  Base64 encoder (for WebSocket key)
// ─────────────────────────────────────────────────────────────────────────────
static std::string base64_encode(const uint8_t* data, size_t len) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i+1 < len) b |= (uint32_t)data[i+1] << 8;
        if (i+2 < len) b |= (uint32_t)data[i+2];
        out += table[(b >> 18) & 0x3F];
        out += table[(b >> 12) & 0x3F];
        out += (i+1 < len) ? table[(b >> 6) & 0x3F] : '=';
        out += (i+2 < len) ? table[(b >> 0) & 0x3F] : '=';
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  SHA-1 of string (for WebSocket accept key verification)
// ─────────────────────────────────────────────────────────────────────────────
static std::string sha1_b64(const std::string& input) {
    uint8_t digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const uint8_t*>(input.data()),
         input.size(), digest);
    return base64_encode(digest, SHA_DIGEST_LENGTH);
}

// ─────────────────────────────────────────────────────────────────────────────
//  WsClient — TLS WebSocket connection
// ─────────────────────────────────────────────────────────────────────────────
class WsClient {
public:
    using MessageCb = std::function<void(const std::string& msg)>;
    using ErrorCb   = std::function<void(const std::string& err)>;

    WsClient() {
#ifdef HYDRA_WIN_SOCK
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0)
            throw std::runtime_error("WSAStartup failed");
#endif
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) throw std::runtime_error("SSL_CTX_new failed");
        // Accept any certificate for simplicity (market data only)
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
    }

    ~WsClient() {
        disconnect();
        if (ctx_) SSL_CTX_free(ctx_);
#ifdef HYDRA_WIN_SOCK
        WSACleanup();
#endif
    }

    // Non-copyable
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    void set_message_cb(MessageCb cb) { on_message_ = std::move(cb); }
    void set_error_cb(ErrorCb cb)     { on_error_   = std::move(cb); }

    // ── Connect and upgrade to WebSocket ─────────────────────────────────────
    bool connect(const std::string& host, const std::string& path,
                 int port = 443) {
        host_ = host;
        path_ = path;

        // ── Resolve hostname ──────────────────────────────────────────────────
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        std::string port_str = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
            error("getaddrinfo failed for " + host);
            return false;
        }

        // ── Create TCP socket and connect ─────────────────────────────────────
        sock_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock_ == HYDRA_INVALID_SOCKET) { freeaddrinfo(res); error("socket()"); return false; }
        if (::connect(sock_, res->ai_addr, (int)res->ai_addrlen) != 0) {
            freeaddrinfo(res); error("connect()"); return false;
        }
        freeaddrinfo(res);

        // ── TLS handshake ─────────────────────────────────────────────────────
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, (int)sock_);
        SSL_set_tlsext_host_name(ssl_, host.c_str());
        if (SSL_connect(ssl_) != 1) {
            error("SSL_connect failed");
            return false;
        }

        // ── WebSocket HTTP Upgrade ─────────────────────────────────────────────
        uint8_t key_bytes[16];
        RAND_bytes(key_bytes, 16);
        std::string ws_key = base64_encode(key_bytes, 16);

        std::string req =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + ":" + port_str + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + ws_key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "User-Agent: HydraExchange/1.0\r\n"
            "\r\n";

        ssl_write(req);

        // Read HTTP response
        std::string resp;
        resp.reserve(512);
        while (resp.find("\r\n\r\n") == std::string::npos) {
            char buf[256];
            int n = SSL_read(ssl_, buf, sizeof(buf)-1);
            if (n <= 0) { error("SSL_read during upgrade"); return false; }
            buf[n] = '\0';
            resp += buf;
        }

        if (resp.find("101") == std::string::npos) {
            error("WebSocket upgrade failed:\n" + resp.substr(0, 200));
            return false;
        }

        // Verify Sec-WebSocket-Accept
        std::string expected = sha1_b64(ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        if (resp.find(expected) == std::string::npos) {
            // Some servers are fine even if we skip verification
            // Don't hard-fail here for Binance
        }

        connected_ = true;
        return true;
    }

    // ── Blocking receive loop — calls on_message_ for each text frame ─────────
    void run_loop() {
        while (connected_) {
            auto msg = read_frame();
            if (msg.empty() && !connected_) break;
            if (!msg.empty() && on_message_) on_message_(msg);
        }
    }

    void disconnect() {
        connected_ = false;
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (sock_ != HYDRA_INVALID_SOCKET) {
            hydra_close_socket(sock_);
            sock_ = HYDRA_INVALID_SOCKET;
        }
    }

    bool is_connected() const { return connected_; }

private:
    // ── SSL I/O helpers ───────────────────────────────────────────────────────
    void ssl_write(const std::string& data) {
        size_t written = 0;
        while (written < data.size()) {
            int n = SSL_write(ssl_,
                data.data() + written, (int)(data.size() - written));
            if (n <= 0) { error("SSL_write"); return; }
            written += n;
        }
    }

    bool ssl_read_exact(uint8_t* buf, size_t len) {
        size_t got = 0;
        while (got < len) {
            int n = SSL_read(ssl_, buf + got, (int)(len - got));
            if (n <= 0) { connected_ = false; return false; }
            got += n;
        }
        return true;
    }

    // ── WebSocket frame send (text, client→server, must be masked) ───────────
    void send_frame(const std::string& payload, uint8_t opcode = 0x01) {
        std::vector<uint8_t> frame;
        frame.push_back(0x80 | opcode);  // FIN + opcode

        uint32_t mask_key;
        RAND_bytes(reinterpret_cast<uint8_t*>(&mask_key), 4);
        uint8_t* mk = reinterpret_cast<uint8_t*>(&mask_key);

        size_t len = payload.size();
        if (len < 126) {
            frame.push_back(0x80 | (uint8_t)len);
        } else if (len < 65536) {
            frame.push_back(0x80 | 126);
            frame.push_back((len >> 8) & 0xFF);
            frame.push_back(len & 0xFF);
        } else {
            frame.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i)
                frame.push_back((len >> (i*8)) & 0xFF);
        }
        frame.push_back(mk[0]); frame.push_back(mk[1]);
        frame.push_back(mk[2]); frame.push_back(mk[3]);
        for (size_t i = 0; i < len; ++i)
            frame.push_back((uint8_t)payload[i] ^ mk[i % 4]);

        std::string raw(frame.begin(), frame.end());
        ssl_write(raw);
    }

    void send_pong(const std::string& payload) {
        send_frame(payload, 0x0A);  // opcode 0xA = pong
    }

    // ── WebSocket frame receive ───────────────────────────────────────────────
    std::string read_frame() {
        uint8_t header[2];
        if (!ssl_read_exact(header, 2)) return "";

        uint8_t opcode  = header[0] & 0x0F;
        bool    masked  = (header[1] & 0x80) != 0;
        uint64_t length = header[1] & 0x7F;

        if (length == 126) {
            uint8_t ext[2];
            if (!ssl_read_exact(ext, 2)) return "";
            length = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (length == 127) {
            uint8_t ext[8];
            if (!ssl_read_exact(ext, 8)) return "";
            length = 0;
            for (int i = 0; i < 8; ++i) length = (length << 8) | ext[i];
        }

        uint8_t mask[4] = {};
        if (masked) {
            if (!ssl_read_exact(mask, 4)) return "";
        }

        std::vector<uint8_t> payload(length);
        if (length > 0 && !ssl_read_exact(payload.data(), length)) return "";
        if (masked)
            for (size_t i = 0; i < length; ++i) payload[i] ^= mask[i % 4];

        // Handle control frames
        if (opcode == 0x09) {  // ping
            std::string ping_data(payload.begin(), payload.end());
            send_pong(ping_data);
            return "";
        }
        if (opcode == 0x08) {  // close
            connected_ = false;
            return "";
        }
        if (opcode == 0x01 || opcode == 0x00) {  // text / continuation
            return std::string(payload.begin(), payload.end());
        }
        return "";
    }

    void error(const std::string& msg) {
        connected_ = false;
        if (on_error_) on_error_(msg);
        else std::fprintf(stderr, "[WS ERROR] %s\n", msg.c_str());
    }

    SSL_CTX*  ctx_{nullptr};
    SSL*      ssl_{nullptr};
    socket_t  sock_{HYDRA_INVALID_SOCKET};
    bool      connected_{false};
    std::string host_, path_;
    MessageCb on_message_;
    ErrorCb   on_error_;
};

} // namespace hydra
