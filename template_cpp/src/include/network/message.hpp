#ifndef MESSAGE_HPP
#define MESSAGE_HPP

#include "common/types.hpp"
#include <vector>
#include <cstdint>

struct Message 
{
    uint32_t sender_id;
    uint32_t seq_number;
    
    Message() : sender_id(0), seq_number(0) {}
    Message(uint32_t sender, uint32_t seq) 
        : sender_id(sender), seq_number(seq) {}
};

// Packet that contains multiple messages (up to 8),type: DATA or ACK
struct Packet 
{
    MessageType type;
    uint32_t sender_id;  // only for DATA packets
    std::vector<uint32_t> seq_numbers;  // message seq numbers or ACK seq numbers
    Packet() : type(MessageType::PERFECT_LINK_DATA), sender_id(0) {}
    
    std::vector<uint8_t> serialize() const;
    static Packet deserialize(const std::vector<uint8_t>& data);
    static Packet createDataPacket(uint32_t sender_id, const std::vector<uint32_t>& seq_numbers);
    static Packet createAckPacket(const std::vector<uint32_t>& seq_numbers);
};
#endif