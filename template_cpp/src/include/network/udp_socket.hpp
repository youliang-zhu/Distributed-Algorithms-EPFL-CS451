#ifndef UDP_SOCKET_HPP
#define UDP_SOCKET_HPP

#include <cstdint>
#include <string>
#include <vector>

class UDPSocket {
public:
    explicit UDPSocket(uint16_t port);
    ~UDPSocket();
    
    void send(const std::string& ip, uint16_t port, const std::vector<uint8_t>& data);
    std::tuple<std::vector<uint8_t>, std::string, uint16_t> receive();
    void close();
    
    uint16_t getPort() const { return port_; }
    int getFd() const { return socket_fd_; }

private:
    int socket_fd_;
    uint16_t port_;
    
    UDPSocket(const UDPSocket&) = delete;
    UDPSocket& operator=(const UDPSocket&) = delete;
};
#endif 