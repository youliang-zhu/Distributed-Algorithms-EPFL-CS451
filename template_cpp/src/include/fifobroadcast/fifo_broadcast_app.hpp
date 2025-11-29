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

struct MessageIdHash {
    std::size_t operator()(const MessageId& id) const {
        return std::hash<uint32_t>()(id.first) ^ (std::hash<uint32_t>()(id.second) << 1);
    }
};

class FIFOBroadcastApp {
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
    
    std::map<uint32_t, milestone1::Sender*> senders_;
    milestone1::Receiver* receiver_;
    UDPSocket* receiver_socket_;
    UDPSocket* sender_socket_;
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
    void handlePacket(const Packet& packet, const std::string& sender_ip, uint16_t sender_port);
    void urbBroadcast(uint32_t sender_id, uint32_t seq);
    void fifoDeliver(uint32_t sender_id, uint32_t seq);
    
    Host findHost(uint32_t id) const;
    uint32_t getProcessIdFromAddress(const std::string& ip, uint16_t port) const;
};

}

#endif
