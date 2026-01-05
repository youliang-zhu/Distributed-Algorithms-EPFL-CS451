// test_common.cpp - Quick test for common modules
// Compile: g++ -std=c++17 -I../include test_common.cpp ../src/common/*.cpp -o test_common
// Run: ./test_common

#include "../src/include/common/types.hpp"
#include "../src/include/common/config.hpp"
#include "../src/include/common/logger.hpp"
#include "../src/include/common/signal_handler.hpp"
#include <iostream>
#include <fstream>
#include <cassert>
#include <thread>
#include <chrono>

void test_config() 
{
    std::cout << "Testing Config...\n";
    
    // Test 1: Perfect Link config
    {
        std::ofstream file("test_pl.config");
        file << "100 3\n";
        file.close();
        
        Config config = Config::parse("test_pl.config");
        assert(config.getType() == ConfigType::PERFECT_LINK);
        assert(config.getPerfectLinkConfig().m == 100);
        assert(config.getPerfectLinkConfig().receiver_id == 3);
        std::cout << "  ✓ Perfect Link config\n";
    }
    
    // Test 2: FIFO Broadcast config
    {
        std::ofstream file("test_fifo.config");
        file << "200\n";
        file.close();
        
        Config config = Config::parse("test_fifo.config");
        assert(config.getType() == ConfigType::FIFO_BROADCAST);
        assert(config.getFIFOBroadcastConfig().m == 200);
        std::cout << "  ✓ FIFO Broadcast config\n";
    }
    
    // Test 3: Lattice Agreement config
    {
        std::ofstream file("test_la.config");
        file << "10 3 5\n";
        file << "1 2\n";
        file << "3 4 5\n";
        file.close();
        
        Config config = Config::parse("test_la.config");
        assert(config.getType() == ConfigType::LATTICE_AGREEMENT);
        assert(config.getLatticeAgreementConfig().proposals == 10);
        assert(config.getLatticeAgreementConfig().max_values == 3);
        assert(config.getLatticeAgreementConfig().distinct_values == 5);
        assert(config.getLatticeAgreementConfig().proposal_sets.size() == 2);
        assert(config.getLatticeAgreementConfig().proposal_sets[0].size() == 2);
        assert(config.getLatticeAgreementConfig().proposal_sets[1].size() == 3);
        std::cout << "  ✓ Lattice Agreement config\n";
    }
}

void test_logger() 
{
    std::cout << "Testing Logger...\n";
    
    {
        Logger logger("test_output.txt");
        
        // Log some events
        logger.logBroadcast(1);
        logger.logBroadcast(2);
        logger.logDelivery(3, 5);
        logger.logDelivery(2, 1);
        
        // Flush to file
        logger.flush();
    }
    
    // Read and verify
    std::ifstream file("test_output.txt");
    std::string line;
    
    std::getline(file, line);
    assert(line == "b 1");
    
    std::getline(file, line);
    assert(line == "b 2");
    
    std::getline(file, line);
    assert(line == "d 3 5");
    
    std::getline(file, line);
    assert(line == "d 2 1");
    
    file.close();
    std::cout << "  ✓ Logger output format\n";
}

void test_signal_handler() 
{
    std::cout << "Testing SignalHandler...\n";
    
    SignalHandler::setup();
    
    // Initially should not stop
    assert(!SignalHandler::shouldStop());
    std::cout << "  ✓ Initial state\n";
    
    // Note: Can't easily test signal handling without actually sending signals
    // Manual test: run and press Ctrl+C to verify
    std::cout << "  ⚠ Signal handling requires manual test (press Ctrl+C when running main program)\n";
}

void test_types() 
{
    std::cout << "Testing Types...\n";
    
    // Test Host
    Host h1(1, "localhost", 11001);
    assert(h1.id == 1);
    assert(h1.ip == "localhost");
    assert(h1.port == 11001);
    std::cout << "  ✓ Host structure\n";
    
    // Test MessageType enum
    MessageType mt = MessageType::PERFECT_LINK_DATA;
    assert(static_cast<uint8_t>(mt) == 0x01);
    std::cout << "  ✓ MessageType enum\n";
    
    // Test Constants
    assert(Constants::MAX_SEQ_NUMBER == 2147483647);
    assert(Constants::MAX_MESSAGES_PER_PACKET == 8);
    std::cout << "  ✓ Constants\n";
}

int main() {
    std::cout << "=== Testing Common Modules ===\n\n";
    
    try {
        test_types();
        std::cout << "\n";
        
        test_config();
        std::cout << "\n";
        
        test_logger();
        std::cout << "\n";
        
        test_signal_handler();
        std::cout << "\n";
        
        std::cout << "=== All Tests Passed ✓ ===\n";
        
        // Cleanup test files
        std::remove("test_pl.config");
        std::remove("test_fifo.config");
        std::remove("test_la.config");
        std::remove("test_output.txt");
        
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}