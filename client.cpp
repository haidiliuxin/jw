#include "chat_common.hpp"

#include <mutex>
#include <thread>

namespace {
std::atomic<bool> connected{true};
std::mutex output_mutex;

void receive_loop(SOCKET socket) {
    chat::LineReader reader;
    std::string line;
    while (connected.load()) {
        const auto result = reader.read_line(socket, line);
        if (result != chat::ReadResult::Line) break;
        std::lock_guard<std::mutex> lock(output_mutex);
        if (line.rfind("MSG ", 0) == 0) {
            const auto split = line.find(' ', 4);
            if (split != std::string::npos)
                std::cout << '\n' << line.substr(4, split - 4) << ": " << line.substr(split + 1) << '\n';
        } else if (line.rfind("SYS ", 0) == 0) {
            std::cout << "\n[系统] " << line.substr(4) << '\n';
        } else if (line.rfind("USERS ", 0) == 0) {
            std::cout << "\n[在线用户] " << line.substr(6) << '\n';
        } else if (line.rfind("HELP ", 0) == 0) {
            std::cout << "\n[帮助] " << line.substr(5) << '\n';
        } else if (line.rfind("ERR ", 0) == 0) {
            std::cout << "\n[错误] " << line.substr(4) << '\n';
        } else {
            std::cout << '\n' << line << '\n';
        }
        std::cout << "> " << std::flush;
    }
    connected = false;
    std::lock_guard<std::mutex> lock(output_mutex);
    std::cout << "\n与服务器的连接已关闭。\n";
}
}  // namespace

int main(int argc, char* argv[]) {
    chat::configure_console_utf8();
    if (argc > 4) {
        std::cerr << "用法: client.exe [服务器IP] [端口] [用户名]\n";
        return 2;
    }
    const std::string host = argc >= 2 ? argv[1] : "127.0.0.1";
    int port = chat::kDefaultPort;
    if (argc >= 3 && !chat::parse_port(argv[2], port)) {
        std::cerr << "端口必须在 1-65535 之间。\n";
        return 2;
    }
    std::string username;
    if (argc >= 4) username = argv[3];
    else {
        std::cout << "请输入用户名（1-24 字节，不含空格）: ";
        std::getline(std::cin, username);
    }
    if (!chat::valid_username(username)) {
        std::cerr << "用户名须为 1-24 字节且不能含空白。\n";
        return 2;
    }

    chat::WinsockSession winsock;
    if (!winsock.ok()) {
        std::cerr << chat::socket_error("WSAStartup") << '\n';
        return 1;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string port_text = std::to_string(port);
    const int lookup = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses);
    if (lookup != 0) {
        std::cerr << "无法解析服务器地址: " << host << '\n';
        return 1;
    }
    SOCKET socket = INVALID_SOCKET;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket != INVALID_SOCKET && ::connect(socket, address->ai_addr,
                static_cast<int>(address->ai_addrlen)) == 0) break;
        if (socket != INVALID_SOCKET) ::closesocket(socket);
        socket = INVALID_SOCKET;
    }
    ::freeaddrinfo(addresses);
    if (socket == INVALID_SOCKET) {
        std::cerr << chat::socket_error("connect") << '\n';
        return 1;
    }

    chat::LineReader handshake;
    if (!chat::send_line(socket, "JOIN " + username)) {
        std::cerr << "发送登录请求失败。\n";
        ::closesocket(socket);
        return 1;
    }
    std::string response;
    if (handshake.read_line(socket, response) != chat::ReadResult::Line || response.rfind("OK ", 0) != 0) {
        std::cerr << "无法加入聊天室: " << response << '\n';
        ::closesocket(socket);
        return 1;
    }
    std::cout << "已连接到 " << host << ':' << port << "，用户名：" << username
              << "。输入文字发送；/who 查看用户；/help 帮助；/quit 退出。\n";

    std::thread receiver(receive_loop, socket);
    std::string input;
    while (connected.load() && std::getline(std::cin, input)) {
        if (input == "/quit") {
            chat::send_line(socket, "QUIT");
            break;
        }
        if (input == "/who") chat::send_line(socket, "WHO");
        else if (input == "/help") chat::send_line(socket, "HELP");
        else if (!input.empty()) chat::send_line(socket, "MSG " + input);
        if (connected.load()) {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cout << "> " << std::flush;
        }
    }
    connected = false;
    ::shutdown(socket, SD_BOTH);
    ::closesocket(socket);
    if (receiver.joinable()) receiver.join();
    return 0;
}
