#include "network/udp_socket.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <tuple>
#include <iostream>

UDPSocket::UDPSocket(uint16_t port) : port_(port) {
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) throw std::runtime_error("Failed to create socket");

    // [Fix 1] 允许端口复用
    int opt = 1;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
    }
    
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(socket_fd_);
        throw std::runtime_error("Failed to bind socket");
    }
}

UDPSocket::~UDPSocket() {
    close();
}

void UDPSocket::close() {
    if (socket_fd_ >= 0) {
        // [Fix 2] Shutdown 强制唤醒 recvfrom
        ::shutdown(socket_fd_, SHUT_RDWR);
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

void UDPSocket::send(const std::string& ip, uint16_t port, const std::vector<uint8_t>& data) 
{
    sockaddr_in dest_addr;
    std::memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip.c_str(), &dest_addr.sin_addr) <= 0) 
    {
        throw std::runtime_error("Invalid IP address");
    }
    
    ssize_t sent = sendto(socket_fd_, data.data(), data.size(), 0,
                          reinterpret_cast<sockaddr*>(&dest_addr), sizeof(dest_addr));
    
    // 在 shutdown 后 send 可能会失败，这是预期的，不抛出异常
    (void)sent;
}

std::tuple<std::vector<uint8_t>, std::string, uint16_t> UDPSocket::receive() 
{
    std::vector<uint8_t> buffer(65536);
    sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);
    
    ssize_t received = recvfrom(socket_fd_, buffer.data(), buffer.size(), 0,
                                reinterpret_cast<sockaddr*>(&sender_addr), &addr_len);

    if (received < 0) 
    {
        throw std::runtime_error("Failed to receive data");
    }
    buffer.resize(received);
    
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    uint16_t sender_port = ntohs(sender_addr.sin_port);
    
    // [DIAG] 埋点：监控物理接收
    // 这能证明数据包是否真的到达了进程的 Socket 缓冲区
    std::cout << "[DIAG-NET] Socket " << port_ << " RECV " << received << " bytes from " << ip_str << ":" << sender_port << std::endl;
    
    return std::make_tuple(buffer, std::string(ip_str), sender_port);
}