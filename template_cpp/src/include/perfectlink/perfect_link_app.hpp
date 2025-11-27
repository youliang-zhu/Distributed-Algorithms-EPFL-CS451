#ifndef PERFECT_LINK_APP_HPP
#define PERFECT_LINK_APP_HPP

#include "common/types.hpp"
#include "common/logger.hpp"
#include "network/udp_socket.hpp"
#include "network/message.hpp"
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <map>
#include <set>
#include <chrono>
#include <condition_variable>

namespace milestone1 
{

struct SentMessage 
{
    uint32_t seq_number;
    std::chrono::steady_clock::time_point last_sent;
    uint32_t retransmit_count;
    
    SentMessage() : seq_number(0), retransmit_count(0) {}
    SentMessage(uint32_t seq, std::chrono::steady_clock::time_point time)
        : seq_number(seq), last_sent(time), retransmit_count(0) {}
};

struct TimeoutEntry 
{
    std::chrono::steady_clock::time_point timeout_time;
    uint32_t seq_number;
    
    bool operator>(const TimeoutEntry& other) const {
        return timeout_time > other.timeout_time;
    }
};

class Sender 
{
public:
    Sender(UDPSocket* socket, uint32_t my_id, const Host& receiver, Logger* logger);
    ~Sender();
    
    void start();
    void stop();
    void send(uint32_t seq_number);
    
    void waitUntilAllAcked();
    bool allMessagesAcked() const;

private:
    UDPSocket* socket_;
    uint32_t my_id_;
    Host receiver_;
    Logger* logger_;
    
    std::queue<uint32_t> pending_queue_;
    std::map<uint32_t, SentMessage> unacked_messages_;
    std::priority_queue<TimeoutEntry, std::vector<TimeoutEntry>, std::greater<>> timeout_queue_;
    
    mutable std::mutex queue_mutex_;
    mutable std::mutex data_mutex_;
    std::condition_variable queue_cv_;
    std::condition_variable timeout_cv_;
    
    std::thread send_thread_;
    std::thread retransmit_thread_;
    std::thread ack_receive_thread_;
    std::atomic<bool> running_;
    
    static constexpr std::chrono::milliseconds TIMEOUT{50};
    static constexpr size_t MAX_BATCH_SIZE = 16;
    
    void sendLoop();
    void retransmitLoop();
    void ackReceiveLoop();
};

class Receiver 
{
public:
    Receiver(UDPSocket* socket, Logger* logger);
    ~Receiver();
    
    void start();
    void stop();
    void handle(const Packet& packet, const std::string& sender_ip, uint16_t sender_port);
    void flushAllPendingAcks();

private:
    void flushLoop();

    UDPSocket* socket_;
    Logger* logger_;
    
    std::map<uint32_t, std::set<uint32_t>> delivered_messages_;
    std::map<std::string, std::vector<uint32_t>> pending_acks_;
    
    std::mutex mtx_;
    std::thread flush_thread_;
    std::atomic<bool> flush_running_;
    
    static constexpr size_t MAX_DELIVERED_WINDOW = 10000;
    static constexpr size_t ACK_BATCH_SIZE = 8;
    static constexpr std::chrono::milliseconds ACK_FLUSH_TIMEOUT{1};
};

class PerfectLinkApp 
{
public:
    PerfectLinkApp(uint32_t my_id, const std::vector<Host>& hosts,
                   uint32_t m, uint32_t receiver_id, const std::string& output_path);
    ~PerfectLinkApp();
    
    void run();
    void shutdown();
    bool isSender() const { return sender_ != nullptr; }

private:
    uint32_t my_id_;
    std::vector<Host> hosts_;
    uint32_t m_;
    uint32_t receiver_id_;
    
    UDPSocket* receiver_socket_;
    UDPSocket* sender_socket_;
    Sender* sender_;
    Receiver* receiver_;
    Logger* logger_;
    
    std::thread receive_thread_;
    std::atomic<bool> running_;
    
    void receiveLoop();
    Host findHost(uint32_t id) const;
};

}
#endif