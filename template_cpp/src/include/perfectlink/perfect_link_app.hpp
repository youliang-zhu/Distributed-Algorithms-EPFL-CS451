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
#include <vector>
#include <chrono>
#include <condition_variable>
#include <functional>

namespace milestone1 
{

using MessageHandler = std::function<void(uint32_t sender_id, uint32_t seq, const std::string& udp_source_ip, uint16_t udp_source_port)>;

// 常量定义
static constexpr size_t MAX_BATCH_SIZE = 8;
static constexpr std::chrono::milliseconds RETRANSMIT_TIMEOUT{50};
static constexpr std::chrono::milliseconds ACK_FLUSH_TIMEOUT{10}; 
// PL 层去重窗口限制
static constexpr size_t MAX_DELIVERED_WINDOW = 20000; 

// --- Unified Sender Structures ---
struct PendingMessage {
    uint32_t target_id;
    uint32_t original_sender_id;
    uint32_t seq_number;
};

struct UnackedMessage {
    uint32_t original_sender_id;
    uint32_t seq_number;
    std::chrono::steady_clock::time_point last_sent;
};

struct TimeoutEntry {
    std::chrono::steady_clock::time_point timeout_time;
    uint32_t target_id;
    uint32_t original_sender_id;
    uint32_t seq_number;
    
    bool operator>(const TimeoutEntry& other) const {
        return timeout_time > other.timeout_time;
    }
};

class UnifiedSender 
{
public:
    UnifiedSender(UDPSocket* socket, const std::vector<Host>& neighbors, Logger* logger);
    ~UnifiedSender();
    
    void start();
    void stop();
    
    void send(uint32_t target_id, uint32_t original_sender_id, uint32_t seq_number);
    void processAck(uint32_t source_id, const std::vector<AckItem>& acks);

private:
    UDPSocket* socket_;
    std::map<uint32_t, Host> routing_table_;
    Logger* logger_;
    
    std::queue<PendingMessage> send_queue_;
    
    using MsgKey = std::pair<uint32_t, uint32_t>; // <OrigSender, Seq>
    std::map<uint32_t, std::map<MsgKey, UnackedMessage>> unacked_window_;
    
    std::priority_queue<TimeoutEntry, std::vector<TimeoutEntry>, std::greater<TimeoutEntry>> timeout_queue_;
    
    std::mutex mutex_;
    std::condition_variable cv_send_;
    std::condition_variable cv_retransmit_;
    
    std::thread send_thread_;
    std::thread retransmit_thread_;
    std::atomic<bool> running_;
    
    void sendLoop();
    void retransmitLoop();
};

class Receiver 
{
public:
    Receiver(UDPSocket* socket, Logger* logger);
    ~Receiver();
    
    void start();
    void stop();
    
    void handleData(const Packet& packet, const std::string& sender_ip, uint16_t sender_port);
    void setMessageHandler(MessageHandler handler);

private:
    void flushLoop();

    UDPSocket* socket_;
    Logger* logger_;
    MessageHandler message_handler_;
    
    std::map<uint32_t, std::set<uint32_t>> delivered_;
    std::map<std::string, std::vector<AckItem>> pending_acks_;
    
    std::mutex mtx_;
    std::thread flush_thread_;
    std::atomic<bool> flush_running_;
};

// [新增] PerfectLinkApp 类，为了兼容 main.cpp
class PerfectLinkApp 
{
public:
    PerfectLinkApp(uint32_t my_id, const std::vector<Host>& hosts,
                   uint32_t m, uint32_t receiver_id, const std::string& output_path);
    ~PerfectLinkApp();
    
    void run();
    void shutdown();
    bool isSender() const; // main.cpp 需要此接口

private:
    uint32_t my_id_;
    std::vector<Host> hosts_;
    uint32_t m_;
    uint32_t receiver_id_;
    
    // 使用新架构组件
    UDPSocket* socket_;
    UnifiedSender* unified_sender_;
    Receiver* receiver_;
    Logger* logger_;
    
    std::thread receive_thread_;
    std::atomic<bool> running_;
    
    void receiveLoop();
    Host findHost(uint32_t id) const;
    uint32_t getProcessIdFromAddress(const std::string& ip, uint16_t port) const;
};

}
#endif