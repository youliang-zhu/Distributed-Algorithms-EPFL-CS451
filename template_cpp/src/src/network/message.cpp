#include "network/message.hpp"
#include <cstring>

static void write_uint32(std::vector<uint8_t>& buffer, uint32_t value) {
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}
static uint32_t read_uint32(const std::vector<uint8_t>& buffer, size_t& pos) {
    uint32_t value = 0;
    value |= static_cast<uint32_t>(buffer[pos++]) << 24;
    value |= static_cast<uint32_t>(buffer[pos++]) << 16;
    value |= static_cast<uint32_t>(buffer[pos++]) << 8;
    value |= static_cast<uint32_t>(buffer[pos++]);
    return value;
}

std::vector<uint8_t> Packet::serialize() const 
{
    std::vector<uint8_t> buffer;
    buffer.push_back(static_cast<uint8_t>(type));
    if (type == MessageType::PERFECT_LINK_DATA) 
    {
        write_uint32(buffer, sender_id);
    }
    buffer.push_back(static_cast<uint8_t>(seq_numbers.size()));
    for (uint32_t seq : seq_numbers) {
        write_uint32(buffer, seq);
    }
    return buffer;
}

Packet Packet::deserialize(const std::vector<uint8_t>& data) 
{
    Packet packet;
    size_t pos = 0;
    
    packet.type = static_cast<MessageType>(data[pos++]);
    if (packet.type == MessageType::PERFECT_LINK_DATA) 
    {
        packet.sender_id = read_uint32(data, pos);
    }
    uint8_t count = data[pos++];

    for (uint8_t i = 0; i < count; i++) 
    {
        uint32_t seq = read_uint32(data, pos);
        packet.seq_numbers.push_back(seq);
    }
    
    return packet;
}

Packet Packet::createDataPacket(uint32_t sender_id, const std::vector<uint32_t>& seq_numbers) 
{
    Packet packet;
    packet.type = MessageType::PERFECT_LINK_DATA;
    packet.sender_id = sender_id;
    packet.seq_numbers = seq_numbers;
    return packet;
}

Packet Packet::createAckPacket(const std::vector<uint32_t>& seq_numbers) 
{
    Packet packet;
    packet.type = MessageType::PERFECT_LINK_ACK;
    packet.sender_id = 0;  // Not used for ACK
    packet.seq_numbers = seq_numbers;
    return packet;
}