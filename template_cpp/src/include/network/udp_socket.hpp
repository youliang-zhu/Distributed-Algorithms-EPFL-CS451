#ifndef UDP_SOCKET_HPP
#define UDP_SOCKET_HPP

#include <cstdint>
#include <string>
#include <vector>

class UDPSocket {
public:
    // Create and bind socket to the specified port
    explicit UDPSocket(uint16_t port);
    
    ~UDPSocket();
    
    // Send data to the specified destination
    void send(const std::string& ip, uint16_t port, const std::vector<uint8_t>& data);
    
    // Receive data (blocking)
    // Returns: received data, sender_ip, sender_port
    std::tuple<std::vector<uint8_t>, std::string, uint16_t> receive();
    
    // Get the bound port
    uint16_t getPort() const { return port_; }

private:
    int socket_fd_;
    uint16_t port_;
    
    // Prevent copying
    UDPSocket(const UDPSocket&) = delete;
    UDPSocket& operator=(const UDPSocket&) = delete;
};

#endif // UDP_SOCKET_HPP