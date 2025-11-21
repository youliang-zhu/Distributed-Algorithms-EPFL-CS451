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
#include <deque>
#include <unordered_set>

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

class Sender 
{
public:
    Sender(UDPSocket* socket, uint32_t my_id, const Host& receiver, Logger* logger);
    ~Sender();
    
    void start();
    void stop();
    void send(uint32_t seq_number);
    void handleAck(const std::vector<uint32_t>& ack_seqs);
    
    void waitUntilAllAcked();
    bool allMessagesAcked() const;

private:
    UDPSocket* socket_;
    uint32_t my_id_;
    Host receiver_;
    Logger* logger_;
    
    // 待发送队列
    std::queue<uint32_t> pending_queue_;
    std::map<uint32_t, SentMessage> unacked_messages_;
    
    std::thread send_thread_;
    std::atomic<bool> running_;
    mutable std::mutex mtx_;
    
    static constexpr std::chrono::milliseconds TIMEOUT{100};
    static constexpr size_t MAX_BATCH_SIZE = 32;
    
    // 线程的主循环
    void sendLoop();
    void sendNewMessages();
    void retransmitTimedOut();
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
    void flushAcks(const std::string& sender_ip, uint16_t sender_port);
    void flushLoop();

    UDPSocket* socket_;
    Logger* logger_;
    
    std::map<uint32_t, std::unordered_set<uint32_t>> delivered_messages_;
    static constexpr size_t MAX_DELIVERED_WINDOW = 10000;
    mutable std::mutex mtx_;

    std::map<std::string, std::vector<uint32_t>> pending_acks_;
    std::chrono::steady_clock::time_point last_ack_time_;
    static constexpr std::chrono::milliseconds ACK_FLUSH_INTERVAL{5};
    static constexpr size_t MAX_ACKS_PER_PACKET = 32;

    // 定期flush线程
    std::thread flush_thread_;
    std::atomic<bool> flush_running_;
};

class PerfectLinkApp 
{
public:
    PerfectLinkApp(uint32_t my_id, const std::vector<Host>& hosts,
                   uint32_t m, uint32_t receiver_id, const std::string& output_path);
    ~PerfectLinkApp();
    
    void run();
    void shutdown();

private:
    uint32_t my_id_;
    std::vector<Host> hosts_;
    uint32_t m_;
    uint32_t receiver_id_;
    
    UDPSocket* socket_;
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