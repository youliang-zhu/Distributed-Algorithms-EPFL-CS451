#ifndef TYPES_HPP
#define TYPES_HPP

#include <cstdint>
#include <string>
#include <vector>

struct Host {
    uint32_t id;
    std::string ip;
    uint16_t port;
    
    Host() : id(0), port(0) {}
    Host(uint32_t id, const std::string& ip, uint16_t port) 
        : id(id), ip(ip), port(port) {}
};

enum class MessageType : uint8_t {
    PERFECT_LINK_DATA = 0x01,
    PERFECT_LINK_ACK  = 0x02,
    
    BROADCAST_DATA = 0x11,
    BROADCAST_ACK  = 0x12,
    
    PROPOSAL = 0x21,
    NACK     = 0x22
};

struct PerfectLinkConfig {
    uint32_t m;
    uint32_t receiver_id;
    
    PerfectLinkConfig() : m(0), receiver_id(0) {}
    PerfectLinkConfig(uint32_t m, uint32_t receiver_id) 
        : m(m), receiver_id(receiver_id) {}
};

struct FIFOBroadcastConfig {
    uint32_t m;
    
    FIFOBroadcastConfig() : m(0) {}
    explicit FIFOBroadcastConfig(uint32_t m) : m(m) {}
};

struct LatticeAgreementConfig {
    uint32_t proposals;
    uint32_t max_values;
    uint32_t distinct_values;
    std::vector<std::vector<uint32_t>> proposal_sets;
    
    LatticeAgreementConfig() 
        : proposals(0), max_values(0), distinct_values(0) {}
};

namespace Constants {
    constexpr uint32_t MAX_SEQ_NUMBER = 2147483647;  // 2^31 - 1
    constexpr size_t MAX_MESSAGES_PER_PACKET = 8;
    constexpr size_t MAX_UDP_PACKET_SIZE = 65507;    // 65535 - 8 (UDP header) - 20 (IP header)
}

#endif