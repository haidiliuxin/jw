#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif

// 公共 Winsock 与行协议工具：只使用原生 Winsock API。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>

namespace chat {
constexpr int kDefaultPort = 54000;
constexpr std::size_t kMaxFrameBytes = 4096;
constexpr int kMaxClients = 64;

class WinsockSession {
public:
    WinsockSession() : ok_(WSAStartup(MAKEWORD(2, 2), &data_) == 0) {}
    ~WinsockSession() { if (ok_) WSACleanup(); }
    bool ok() const { return ok_; }
    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;
private:
    WSADATA data_{};
    bool ok_;
};

inline void configure_console_utf8() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
}

inline bool send_all(SOCKET socket, const char* data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        const int n = ::send(socket, data + sent,
            static_cast<int>(std::min<std::size_t>(length - sent, 1u << 20)), 0);
        if (n == SOCKET_ERROR || n == 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

inline bool send_line(SOCKET socket, const std::string& line) {
    if (line.size() > kMaxFrameBytes || line.find('\n') != std::string::npos ||
        line.find('\r') != std::string::npos) return false;
    std::string frame = line;
    frame.push_back('\n');
    return send_all(socket, frame.data(), frame.size());
}

enum class ReadResult { Line, Closed, TooLong, Error };

// 每个连接专属的读取器，保留一次 recv 中多出来的完整帧/半帧。
class LineReader {
public:
    ReadResult read_line(SOCKET socket, std::string& line) {
        for (;;) {
            const auto newline = pending_.find('\n');
            if (newline != std::string::npos) {
                if (newline > kMaxFrameBytes) return ReadResult::TooLong;
                line.assign(pending_, 0, newline);
                pending_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return ReadResult::Line;
            }
            if (pending_.size() > kMaxFrameBytes) return ReadResult::TooLong;
            char buffer[1024];
            const int n = ::recv(socket, buffer, sizeof(buffer), 0);
            if (n == 0) return ReadResult::Closed;
            if (n == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEINTR) continue;
                return ReadResult::Error;
            }
            pending_.append(buffer, static_cast<std::size_t>(n));
        }
    }
private:
    std::string pending_;
};

inline bool valid_username(const std::string& name) {
    if (name.empty() || name.size() > 24) return false;
    for (unsigned char ch : name) if (ch <= 0x20 || ch == 0x7f) return false;
    return true;
}

inline bool parse_port(const char* text, int& port) {
    if (text == nullptr || *text == '\0') return false;
    try {
        std::size_t used = 0;
        const int value = std::stoi(text, &used);
        if (used != std::string(text).size() || value < 1 || value > 65535) return false;
        port = value;
        return true;
    } catch (...) { return false; }
}

inline std::string socket_error(const char* action) {
    return std::string(action) + " 失败，Winsock 错误码=" + std::to_string(WSAGetLastError());
}
}  // namespace chat
