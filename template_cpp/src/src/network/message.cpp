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

std::vector<uint8_t> Packet::serialize() const {
    std::vector<uint8_t> buffer;
    buffer.push_back(static_cast<uint8_t>(type));

    if (type == MessageType::PERFECT_LINK_DATA) {
        write_uint32(buffer, sender_id);
        buffer.push_back(static_cast<uint8_t>(seq_numbers.size()));
        for (uint32_t seq : seq_numbers) {
            write_uint32(buffer, seq);
        }
    }
    else if (type == MessageType::PERFECT_LINK_ACK) {
        buffer.push_back(static_cast<uint8_t>(ack_payloads.size()));
        for (const auto& item : ack_payloads) {
            write_uint32(buffer, item.sender_id);
            write_uint32(buffer, item.seq_number);
        }
    }
    else if (type == MessageType::PROPOSAL) {
        // PROPOSAL: slot + pn + sender + set
        write_uint32(buffer, slot_number);
        write_uint32(buffer, proposal_number);
        write_uint32(buffer, sender_id);
        buffer.push_back(static_cast<uint8_t>(value_set.size()));
        for (uint32_t val : value_set) {
            write_uint32(buffer, val);
        }
    }
    else if (type == MessageType::ACK) {
        // ACK: slot + pn
        write_uint32(buffer, slot_number);
        write_uint32(buffer, proposal_number);
    }
    else if (type == MessageType::NACK) {
        // NACK: slot + pn + set
        write_uint32(buffer, slot_number);
        write_uint32(buffer, proposal_number);
        buffer.push_back(static_cast<uint8_t>(value_set.size()));
        for (uint32_t val : value_set) {
            write_uint32(buffer, val);
        }
    }
    return buffer;
}

Packet Packet::deserialize(const std::vector<uint8_t>& data) {
    Packet packet;
    size_t pos = 0;

    if (pos >= data.size()) return packet;

    packet.type = static_cast<MessageType>(data[pos++]);

    if (packet.type == MessageType::PERFECT_LINK_DATA) {
        if (pos + 4 <= data.size()) packet.sender_id = read_uint32(data, pos);
        if (pos < data.size()) {
            uint8_t count = data[pos++];
            for (uint8_t i = 0; i < count && pos + 4 <= data.size(); i++) {
                packet.seq_numbers.push_back(read_uint32(data, pos));
            }
        }
    }
    else if (packet.type == MessageType::PERFECT_LINK_ACK) {
        if (pos < data.size()) {
            uint8_t count = data[pos++];
            for (uint8_t i = 0; i < count && pos + 8 <= data.size(); i++) {
                uint32_t sid = read_uint32(data, pos);
                uint32_t seq = read_uint32(data, pos);
                packet.ack_payloads.push_back({sid, seq});
            }
        }
    }
    else if (packet.type == MessageType::PROPOSAL) {
        if (pos + 12 <= data.size()) {
            packet.slot_number = read_uint32(data, pos);
            packet.proposal_number = read_uint32(data, pos);
            packet.sender_id = read_uint32(data, pos);
        }
        if (pos < data.size()) {
            uint8_t count = data[pos++];
            for (uint8_t i = 0; i < count && pos + 4 <= data.size(); i++) {
                packet.value_set.insert(read_uint32(data, pos));
            }
        }
    }
    else if (packet.type == MessageType::ACK) {
        if (pos + 8 <= data.size()) {
            packet.slot_number = read_uint32(data, pos);
            packet.proposal_number = read_uint32(data, pos);
        }
    }
    else if (packet.type == MessageType::NACK) {
        if (pos + 8 <= data.size()) {
            packet.slot_number = read_uint32(data, pos);
            packet.proposal_number = read_uint32(data, pos);
        }
        if (pos < data.size()) {
            uint8_t count = data[pos++];
            for (uint8_t i = 0; i < count && pos + 4 <= data.size(); i++) {
                packet.value_set.insert(read_uint32(data, pos));
            }
        }
    }
    return packet;
}

Packet Packet::createDataPacket(uint32_t sender_id, const std::vector<uint32_t>& seq_numbers) {
    Packet packet;
    packet.type = MessageType::PERFECT_LINK_DATA;
    packet.sender_id = sender_id;
    packet.seq_numbers = seq_numbers;
    return packet;
}

Packet Packet::createAckPacket(const std::vector<AckItem>& acks) {
    Packet packet;
    packet.type = MessageType::PERFECT_LINK_ACK;
    packet.ack_payloads = acks;
    return packet;
}

Packet Packet::createProposalPacket(uint32_t slot, uint32_t pn, uint32_t sender, const std::set<uint32_t>& values) {
    Packet packet;
    packet.type = MessageType::PROPOSAL;
    packet.slot_number = slot;
    packet.proposal_number = pn;
    packet.sender_id = sender;
    packet.value_set = values;
    return packet;
}

Packet Packet::createLAAckPacket(uint32_t slot, uint32_t pn) {
    Packet packet;
    packet.type = MessageType::ACK;
    packet.slot_number = slot;
    packet.proposal_number = pn;
    return packet;
}

Packet Packet::createNackPacket(uint32_t slot, uint32_t pn, const std::set<uint32_t>& values) {
    Packet packet;
    packet.type = MessageType::NACK;
    packet.slot_number = slot;
    packet.proposal_number = pn;
    packet.value_set = values;
    return packet;
}