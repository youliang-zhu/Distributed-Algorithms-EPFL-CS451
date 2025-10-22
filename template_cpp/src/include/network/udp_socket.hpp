#ifndef UDP_SOCKET_HPP
#define UDP_SOCKET_HPP

#include <cstdint>
#include <string>
#include <vector>

class UDPSocket {
public:
    // Create and bind socket
    explicit UDPSocket(uint16_t port);
    ~UDPSocket();
    
    void send(const std::string& ip, uint16_t port, const std::vector<uint8_t>& data);

    // return sender mesage, ip, port
    std::tuple<std::vector<uint8_t>, std::string, uint16_t> receive();
    
    uint16_t getPort() const { return port_; }

private:
    int socket_fd_;
    uint16_t port_;
    
    UDPSocket(const UDPSocket&) = delete;
    UDPSocket& operator=(const UDPSocket&) = delete;
};
#endif 