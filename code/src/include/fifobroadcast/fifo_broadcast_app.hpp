#ifndef FIFO_BROADCAST_APP_HPP
#define FIFO_BROADCAST_APP_HPP

#include "common/types.hpp"
#include "common/logger.hpp"
#include "network/udp_socket.hpp"
#include "perfectlink/perfect_link_app.hpp"
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>

namespace milestone2 {

using MessageId = std::pair<uint32_t, uint32_t>;

//URB历史记录窗口限制
static constexpr size_t MAX_URB_WINDOW = 20000; 

class FIFOBroadcastApp 
{
public:
    FIFOBroadcastApp(uint32_t my_id, const std::vector<Host>& hosts,
                     uint32_t m, const std::string& output_path);
    ~FIFOBroadcastApp();
    
    void run();
    void shutdown();

private:
    uint32_t my_id_;
    std::vector<Host> hosts_;
    uint32_t m_;
    uint32_t n_processes_;
    uint32_t majority_;
    
    milestone1::UnifiedSender* unified_sender_;
    milestone1::Receiver* receiver_;
    UDPSocket* socket_;
    Logger* logger_;
    
    std::set<MessageId> forwarded_;
    std::map<MessageId, std::set<uint32_t>> urb_ack_list_;
    std::set<MessageId> urb_delivered_;
    std::map<uint32_t, uint32_t> next_;
    std::map<uint32_t, std::map<uint32_t, MessageId>> pending_;
    
    std::mutex receiver_state_mutex_;
    std::thread receive_thread_;
    std::atomic<bool> running_;
    
    void receiveLoop();
    void onNewPLMessage(uint32_t sender_id, uint32_t seq, const std::string& udp_source_ip, uint16_t udp_source_port);
    void onPLAck(uint32_t sender_id, uint32_t seq, const std::string& udp_source_ip, uint16_t udp_source_port);
    void urbBroadcast(uint32_t sender_id, uint32_t seq);
    void fifoDeliver(uint32_t sender_id, uint32_t seq);
    void tryDeliver(uint32_t sender_id, uint32_t seq);  //检查是否可以交付
    
    Host findHost(uint32_t id) const;
    uint32_t getProcessIdFromAddress(const std::string& ip, uint16_t port) const;
};

}
#endif