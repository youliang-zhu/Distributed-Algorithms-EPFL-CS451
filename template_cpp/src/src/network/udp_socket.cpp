#include "network/udp_socket.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <tuple>

UDPSocket::UDPSocket(uint16_t port) : port_(port) 
{
    // Create UDP socket
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) 
    {
        throw std::runtime_error("Failed to create socket");
    }
    // Bind to port
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) 
    {
        close(socket_fd_);
        throw std::runtime_error("Failed to bind socket");
    }
}

UDPSocket::~UDPSocket() 
{
    if (socket_fd_ >= 0) 
    {
        close(socket_fd_);
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

    if (sent < 0) {
        throw std::runtime_error("Failed to send data");
    }
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
    
    //sender IP and port
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    uint16_t sender_port = ntohs(sender_addr.sin_port);
    
    return std::make_tuple(buffer, std::string(ip_str), sender_port);
}