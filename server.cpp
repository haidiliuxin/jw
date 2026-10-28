#include "chat_common.hpp"

#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
struct Client {
    explicit Client(SOCKET value) : socket(value) {}
    SOCKET socket;
    std::mutex send_mutex;
    std::atomic<bool> closed{false};
    std::string username;
    bool joined = false;
};

std::atomic<bool> running{true};
std::atomic<SOCKET> listen_socket{INVALID_SOCKET};
std::mutex clients_mutex;
std::unordered_map<SOCKET, std::shared_ptr<Client>> clients;
std::vector<std::thread> workers;
std::mutex workers_mutex;

bool safe_send(const std::shared_ptr<Client>& client, const std::string& line) {
    std::lock_guard<std::mutex> lock(client->send_mutex);
    if (client->closed.load()) return false;
    return chat::send_line(client->socket, line);
}

void safe_close(const std::shared_ptr<Client>& client) {
    std::lock_guard<std::mutex> lock(client->send_mutex);
    if (!client->closed.exchange(true)) {
        ::shutdown(client->socket, SD_BOTH);
        ::closesocket(client->socket);
    }
}

std::vector<std::shared_ptr<Client>> snapshot_clients(bool joined_only = false) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    std::vector<std::shared_ptr<Client>> copy;
    copy.reserve(clients.size());
    for (const auto& entry : clients) {
        if (!joined_only || entry.second->joined) copy.push_back(entry.second);
    }
    return copy;
}

void broadcast(const std::string& line) {
    for (const auto& client : snapshot_clients(true)) safe_send(client, line);
}

void remove_client(const std::shared_ptr<Client>& client) {
    bool notify = false;
    std::string username;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        const auto it = clients.find(client->socket);
        if (it != clients.end() && it->second == client) {
            notify = client->joined;
            username = client->username;
            clients.erase(it);
        }
    }
    safe_close(client);
    if (notify && running.load()) broadcast("SYS " + username + " 离开了聊天室");
}

void client_worker(std::shared_ptr<Client> client) {
    chat::LineReader reader;
    std::string line;
    auto result = reader.read_line(client->socket, line);
    if (result != chat::ReadResult::Line || line.rfind("JOIN ", 0) != 0) {
        safe_send(client, "ERR 请先发送 JOIN <用户名>");
        remove_client(client);
        return;
    }

    const std::string username = line.substr(5);
    if (!chat::valid_username(username)) {
        safe_send(client, "ERR 用户名须为 1-24 字节且不能含空白");
        remove_client(client);
        return;
    }
    bool rejected = false;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (const auto& entry : clients) {
            if (entry.second != client && entry.second->joined &&
                entry.second->username == username) {
                rejected = true;
                break;
            }
        }
        if (!rejected && clients.size() > static_cast<std::size_t>(chat::kMaxClients))
            rejected = true;
        if (!rejected) {
            client->username = username;
            client->joined = true;
        }
    }
    if (rejected) {
        safe_send(client, "ERR 用户名重复或聊天室人数已满");
        remove_client(client);
        return;
    }

    if (!safe_send(client, "OK " + username)) {
        remove_client(client);
        return;
    }
    broadcast("SYS " + username + " 加入了聊天室");

    while (running.load()) {
        result = reader.read_line(client->socket, line);
        if (result != chat::ReadResult::Line) break;
        if (line == "QUIT") break;
        if (line == "WHO") {
            std::string users;
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                for (const auto& entry : clients) {
                    if (!entry.second->joined) continue;
                    if (!users.empty()) users += ", ";
                    users += entry.second->username;
                }
            }
            safe_send(client, "USERS " + users);
        } else if (line == "HELP") {
            safe_send(client, "HELP 发送普通文字群聊；/who 查看在线用户；/help 查看帮助；/quit 退出");
        } else if (line.rfind("MSG ", 0) == 0) {
            const std::string text = line.substr(4);
            if (!text.empty()) {
                if (text.size() + username.size() + 5 > chat::kMaxFrameBytes)
                    safe_send(client, "ERR 消息过长，请缩短后重试");
                else
                    broadcast("MSG " + username + " " + text);
            }
        } else {
            safe_send(client, "ERR 未知命令，请发送 /help 查看帮助");
        }
    }
    remove_client(client);
}

void accept_loop() {
    while (running.load()) {
        const SOCKET listener = listen_socket.load();
        if (listener == INVALID_SOCKET) break;
        sockaddr_in address{};
        int address_length = sizeof(address);
        SOCKET accepted = ::accept(listener, reinterpret_cast<sockaddr*>(&address), &address_length);
        if (accepted == INVALID_SOCKET) {
            if (!running.load()) break;
            continue;
        }
        if (!running.load()) { ::closesocket(accepted); break; }
        auto client = std::make_shared<Client>(accepted);
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (clients.size() >= static_cast<std::size_t>(chat::kMaxClients)) {
                ::closesocket(accepted);
                continue;
            }
            clients.emplace(accepted, client);
        }
        std::lock_guard<std::mutex> lock(workers_mutex);
        workers.emplace_back(client_worker, client);
    }
}

void stop_server() {
    if (!running.exchange(false)) return;
    const SOCKET listener = listen_socket.exchange(INVALID_SOCKET);
    if (listener != INVALID_SOCKET) {
        ::shutdown(listener, SD_BOTH);
        ::closesocket(listener);
    }
    for (const auto& client : snapshot_clients()) safe_close(client);
}
}  // namespace

int main(int argc, char* argv[]) {
    chat::configure_console_utf8();
    int port = chat::kDefaultPort;
    if (argc > 2 || (argc == 2 && !chat::parse_port(argv[1], port))) {
        std::cerr << "用法: server.exe [端口(1-65535)]\n";
        return 2;
    }
    chat::WinsockSession winsock;
    if (!winsock.ok()) {
        std::cerr << chat::socket_error("WSAStartup") << '\n';
        return 1;
    }
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::cerr << chat::socket_error("socket") << '\n';
        return 1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<u_short>(port));
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        ::listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << chat::socket_error("bind/listen") << '\n';
        ::closesocket(listener);
        return 1;
    }
    listen_socket = listener;
    std::cout << "服务器已监听 0.0.0.0:" << port << "，最多 " << chat::kMaxClients
              << " 人；输入 /quit 停止服务。\n";
    std::thread accept_thread(accept_loop);
    std::string command;
    while (running.load() && std::getline(std::cin, command)) {
        if (command == "/quit") break;
        if (!command.empty()) std::cout << "服务器命令仅支持 /quit。\n";
    }
    stop_server();
    if (accept_thread.joinable()) accept_thread.join();
    {
        std::lock_guard<std::mutex> lock(workers_mutex);
        for (auto& worker : workers) if (worker.joinable()) worker.join();
    }
    std::cout << "服务器已关闭。\n";
    return 0;
}
