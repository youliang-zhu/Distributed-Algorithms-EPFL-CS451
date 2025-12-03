#ifndef MESSAGE_HPP
#define MESSAGE_HPP

#include "common/types.hpp"
#include <vector>
#include <cstdint>

struct AckItem {
    uint32_t sender_id; // Original Sender ID
    uint32_t seq_number;
};

struct Packet {
    MessageType type;
    
    // DATA Packet
    uint32_t sender_id; // Original Sender ID
    std::vector<uint32_t> seq_numbers; 
    
    // ACK Packet
    std::vector<AckItem> ack_payloads; 

    Packet() : type(MessageType::PERFECT_LINK_DATA), sender_id(0) {}
    
    std::vector<uint8_t> serialize() const;
    static Packet deserialize(const std::vector<uint8_t>& data);
    
    static Packet createDataPacket(uint32_t sender_id, const std::vector<uint32_t>& seq_numbers);
    static Packet createAckPacket(const std::vector<AckItem>& acks);
};
#endif