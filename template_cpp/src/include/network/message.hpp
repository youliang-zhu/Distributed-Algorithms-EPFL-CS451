#ifndef MESSAGE_HPP
#define MESSAGE_HPP

#include "common/types.hpp"
#include <vector>
#include <cstdint>
#include <set>

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

    // Lattice Agreement Packets
    uint32_t slot_number;           // Which instance/slot
    uint32_t proposal_number;       // Iteration number within slot
    std::set<uint32_t> value_set;  // Proposal/accepted values

    Packet() : type(MessageType::PERFECT_LINK_DATA), sender_id(0),
               slot_number(0), proposal_number(0) {}

    std::vector<uint8_t> serialize() const;
    static Packet deserialize(const std::vector<uint8_t>& data);

    static Packet createDataPacket(uint32_t sender_id, const std::vector<uint32_t>& seq_numbers);
    static Packet createAckPacket(const std::vector<AckItem>& acks);

    // Lattice Agreement packet creators
    static Packet createProposalPacket(uint32_t slot, uint32_t pn, uint32_t sender, const std::set<uint32_t>& values);
    static Packet createLAAckPacket(uint32_t slot, uint32_t pn);
    static Packet createNackPacket(uint32_t slot, uint32_t pn, const std::set<uint32_t>& values);
};
#endif