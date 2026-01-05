// test_network.cpp - Test network layer (UDP socket + Message serialization)
// Compile: g++ -std=c++17 -I../include test_network.cpp ../common/*.cpp ../network/*.cpp -o test_network
// 
// Test 1: Message serialization/deserialization
// Test 2: UDP communication between two processes

#include "../include/common/types.hpp"
#include "../include/network/udp_socket.hpp"
#include "../include/network/message.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>

void test_message_serialization() {
    std::cout << "=== Test 1: Message Serialization ===\n";
    
    // Test DATA packet
    {
        std::vector<uint32_t> seqs = {1, 2, 3, 4, 5, 6, 7, 8};
        Packet original = Packet::createDataPacket(123, seqs);
        
        // Serialize
        std::vector<uint8_t> bytes = original.serialize();
        
        std::cout << "DATA packet size: " << bytes.size() << " bytes\n";
        std::cout << "Expected: 1 (type) + 4 (sender) + 1 (count) + 32 (8*4) = 38 bytes\n";
        assert(bytes.size() == 38);
        
        // Deserialize
        Packet decoded = Packet::deserialize(bytes);
        
        assert(decoded.type == MessageType::PERFECT_LINK_DATA);
        assert(decoded.sender_id == 123);
        assert(decoded.seq_numbers.size() == 8);
        assert(decoded.seq_numbers[0] == 1);
        assert(decoded.seq_numbers[7] == 8);
        
        std::cout << "✓ DATA packet serialization/deserialization\n\n";
    }
    
    // Test ACK packet
    {
        std::vector<uint32_t> seqs = {10, 20, 30};
        Packet original = Packet::createAckPacket(seqs);
        
        // Serialize
        std::vector<uint8_t> bytes = original.serialize();
        
        std::cout << "ACK packet size: " << bytes.size() << " bytes\n";
        std::cout << "Expected: 1 (type) + 1 (count) + 12 (3*4) = 14 bytes\n";
        assert(bytes.size() == 14);
        
        // Deserialize
        Packet decoded = Packet::deserialize(bytes);
        
        assert(decoded.type == MessageType::PERFECT_LINK_ACK);
        assert(decoded.seq_numbers.size() == 3);
        assert(decoded.seq_numbers[0] == 10);
        assert(decoded.seq_numbers[1] == 20);
        assert(decoded.seq_numbers[2] == 30);
        
        std::cout << "✓ ACK packet serialization/deserialization\n\n";
    }
}

void test_udp_echo_server() {
    std::cout << "=== Test 2: UDP Echo Server ===\n";
    std::cout << "Starting echo server on port 12000...\n";
    std::cout << "Waiting for messages (will echo back 5 packets then exit)\n\n";
    
    UDPSocket socket(12000);
    
    for (int i = 0; i < 5; i++) {
        // Receive packet
        auto [data, sender_ip, sender_port] = socket.receive();
        
        // Deserialize
        Packet packet = Packet::deserialize(data);
        
        std::cout << "Received packet #" << (i+1) << ":\n";
        std::cout << "  Type: " << (packet.type == MessageType::PERFECT_LINK_DATA ? "DATA" : "ACK") << "\n";
        if (packet.type == MessageType::PERFECT_LINK_DATA) {
            std::cout << "  Sender ID: " << packet.sender_id << "\n";
        }
        std::cout << "  Seq count: " << packet.seq_numbers.size() << "\n";
        std::cout << "  From: " << sender_ip << ":" << sender_port << "\n";
        
        // Echo back (send ACK)
        Packet ack = Packet::createAckPacket(packet.seq_numbers);
        std::vector<uint8_t> ack_bytes = ack.serialize();
        socket.send(sender_ip, sender_port, ack_bytes);
        
        std::cout << "  → Sent ACK back\n\n";
    }
    
    std::cout << "Echo server finished.\n";
}

void test_udp_client() {
    std::cout << "=== Test 2: UDP Client ===\n";
    std::cout << "Starting client on port 12001...\n";
    std::cout << "Sending 5 DATA packets to server...\n\n";
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    UDPSocket socket(12001);
    
    for (int i = 0; i < 5; i++) {
        // Create DATA packet
        std::vector<uint32_t> seqs;
        for (uint32_t j = i * 8 + 1; j <= (i + 1) * 8; j++) {
            seqs.push_back(j);
        }
        
        Packet data_packet = Packet::createDataPacket(999, seqs);
        std::vector<uint8_t> bytes = data_packet.serialize();
        
        // Send to server
        socket.send("127.0.0.1", 12000, bytes);
        std::cout << "Sent packet #" << (i+1) << " with seqs [" << seqs[0] << "-" << seqs[7] << "]\n";
        
        // Wait for ACK
        auto [ack_data, sender_ip, sender_port] = socket.receive();
        Packet ack = Packet::deserialize(ack_data);
        
        assert(ack.type == MessageType::PERFECT_LINK_ACK);
        assert(ack.seq_numbers.size() == 8);
        assert(ack.seq_numbers[0] == seqs[0]);
        
        std::cout << "  ✓ Received ACK\n\n";
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::cout << "Client finished. All packets acknowledged!\n";
}

void print_usage() {
    std::cout << "Usage:\n";
    std::cout << "  ./test_network serialize    - Test message serialization only\n";
    std::cout << "  ./test_network server        - Run UDP echo server\n";
    std::cout << "  ./test_network client        - Run UDP client\n";
    std::cout << "\nFor UDP test, run server in one terminal and client in another.\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    
    std::string mode = argv[1];
    
    try {
        if (mode == "serialize") {
            test_message_serialization();
            std::cout << "=== All Serialization Tests Passed ✓ ===\n";
        } else if (mode == "server") {
            test_udp_echo_server();
        } else if (mode == "client") {
            test_udp_client();
        } else {
            print_usage();
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}