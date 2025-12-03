#include "fifobroadcast/fifo_broadcast_app.hpp"
#include <thread>
#include <chrono>
#include <iterator>

namespace milestone2 
{

FIFOBroadcastApp::FIFOBroadcastApp(uint32_t my_id, const std::vector<Host>& hosts,
                                   uint32_t m, const std::string& output_path)
    : my_id_(my_id), hosts_(hosts), m_(m), running_(false) 
    {
    
    n_processes_ = static_cast<uint32_t>(hosts_.size());
    majority_ = n_processes_ / 2 + 1;
    Host my_host = findHost(my_id_);
    logger_ = new Logger(output_path);
    
    // Single Socket
    socket_ = new UDPSocket(my_host.port);
    unified_sender_ = new milestone1::UnifiedSender(socket_, hosts_, logger_);
    receiver_ = new milestone1::Receiver(socket_, logger_);
    
    //新消息回调：第一次收到消息时触发转发和交付检查
    receiver_->setMessageHandler([this](uint32_t s, uint32_t seq, const std::string& ip, uint16_t port) 
    {
        this->onNewPLMessage(s, seq, ip, port);
    });
    
    //ACK 回调：每次收到消息都触发，用于累积URB ACK
    receiver_->setAckHandler([this](uint32_t s, uint32_t seq, const std::string& ip, uint16_t port) 
    {
        this->onPLAck(s, seq, ip, port);
    });
    
    for (const Host& host : hosts_) next_[host.id] = 1;
}

FIFOBroadcastApp::~FIFOBroadcastApp() 
{
    shutdown();
    delete unified_sender_;
    delete receiver_;
    delete logger_;
    delete socket_;
}

void FIFOBroadcastApp::run() 
{
    running_ = true;
    receive_thread_ = std::thread(&FIFOBroadcastApp::receiveLoop, this);
    receiver_->start();
    unified_sender_->start();
    
    // Cold start wait
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    
    for (uint32_t seq = 1; seq <= m_; seq++) {
        urbBroadcast(my_id_, seq);
    }
    
    logger_->flush();
}

void FIFOBroadcastApp::shutdown() 
{
    running_ = false;
    
    //先关闭Socket触发shutdown
    if (socket_) socket_->close();
    
    if (unified_sender_) unified_sender_->stop();
    if (receiver_) receiver_->stop();
    
    //使用join 安全等待
    if (receive_thread_.joinable()) receive_thread_.join();
    
    logger_->flush();
}

void FIFOBroadcastApp::urbBroadcast(uint32_t sender_id, uint32_t seq) 
{
    MessageId msg_id = {sender_id, seq};
    
    {
        std::lock_guard<std::mutex> lock(receiver_state_mutex_);
        if (sender_id == my_id_) logger_->logBroadcast(seq);
        
        urb_ack_list_[msg_id].insert(my_id_);
        if (sender_id == my_id_) urb_ack_list_[msg_id].insert(sender_id);
    }
    
    for (const auto& host : hosts_) 
    {
        if (host.id != my_id_) {
            unified_sender_->send(host.id, sender_id, seq);
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(receiver_state_mutex_);
        if (urb_delivered_.count(msg_id)) return;
        if (urb_ack_list_[msg_id].size() >= majority_) {
            urb_delivered_.insert(msg_id);
            urb_ack_list_.erase(msg_id);
            fifoDeliver(sender_id, seq);
        }
    }
}

void FIFOBroadcastApp::receiveLoop() 
{
    while (running_) {
        try {
            auto [data, sender_ip, sender_port] = socket_->receive();
            Packet packet = Packet::deserialize(data);
            
            // Dispatch
            if (packet.type == MessageType::PERFECT_LINK_DATA) 
            {
                receiver_->handleData(packet, sender_ip, sender_port);
            } 
            else if (packet.type == MessageType::PERFECT_LINK_ACK) 
            {
                uint32_t source_id = getProcessIdFromAddress(sender_ip, sender_port);
                unified_sender_->processAck(source_id, packet.ack_payloads);
            }
        } catch (...) {
            if (!running_) break;
        }
    }
}

void FIFOBroadcastApp::onNewPLMessage(uint32_t original_sender, uint32_t seq, const std::string& udp_source_ip, uint16_t udp_source_port) {
    MessageId msg_id = {original_sender, seq};
    uint32_t udp_source_id = getProcessIdFromAddress(udp_source_ip, udp_source_port);
    
    bool should_forward = false;
    bool should_deliver = false;
    
    {
        std::lock_guard<std::mutex> lock(receiver_state_mutex_);
        
        //URB: 收到消息后，将发送者和自己都加入 ACK 列表
        urb_ack_list_[msg_id].insert(udp_source_id);//物理发送者
        urb_ack_list_[msg_id].insert(original_sender);//原始发送者
        urb_ack_list_[msg_id].insert(my_id_);//自己也 ACK
        
        //清理 forwarded_集合
        if (forwarded_.size() >= MAX_URB_WINDOW) 
        {
            auto it = forwarded_.begin();
            std::advance(it, MAX_URB_WINDOW / 2);
            forwarded_.erase(forwarded_.begin(), it);
        }

        if (forwarded_.find(msg_id) == forwarded_.end())
        {
            forwarded_.insert(msg_id);
            should_forward = true;
        }
        
        if (!urb_delivered_.count(msg_id) && urb_ack_list_[msg_id].size() >= majority_) 
        {
            // [Fix Memory 3] 清理 urb_delivered_ 集合
            if (urb_delivered_.size() >= MAX_URB_WINDOW) {
                auto it = urb_delivered_.begin();
                std::advance(it, MAX_URB_WINDOW / 2);
                urb_delivered_.erase(urb_delivered_.begin(), it);
            }
            
            urb_delivered_.insert(msg_id);
            urb_ack_list_.erase(msg_id);
            should_deliver = true;
        }
    }
    
    if (should_forward) 
    {
        for (const auto& host : hosts_) 
        {
            if (host.id != my_id_) {
                unified_sender_->send(host.id, original_sender, seq);
            }
        }
    }
    
    if (should_deliver) 
    {
        std::lock_guard<std::mutex> lock(receiver_state_mutex_);
        fifoDeliver(original_sender, seq);
    }
}

//每次收到消息（包括重复的）都会触发，用于累积 ACK
void FIFOBroadcastApp::onPLAck(uint32_t original_sender, uint32_t seq, const std::string& udp_source_ip, uint16_t udp_source_port) 
{
    MessageId msg_id = {original_sender, seq};
    uint32_t udp_source_id = getProcessIdFromAddress(udp_source_ip, udp_source_port);
    
    std::lock_guard<std::mutex> lock(receiver_state_mutex_);
    
    //如果已经交付，忽略
    if (urb_delivered_.count(msg_id)) return;
    
    //添加 ACK
    urb_ack_list_[msg_id].insert(udp_source_id);
    
    // 检查是否可以交付
    if (urb_ack_list_[msg_id].size() >= majority_) 
    {
        if (urb_delivered_.size() >= MAX_URB_WINDOW) 
        {
            auto it = urb_delivered_.begin();
            std::advance(it, MAX_URB_WINDOW / 2);
            urb_delivered_.erase(urb_delivered_.begin(), it);
        }
        
        urb_delivered_.insert(msg_id);
        urb_ack_list_.erase(msg_id);
        fifoDeliver(original_sender, seq);
    }
}

void FIFOBroadcastApp::fifoDeliver(uint32_t sender_id, uint32_t seq) 
{
    if (seq == next_[sender_id]) {
        logger_->logDelivery(sender_id, seq);
        next_[sender_id]++;
        while (pending_[sender_id].count(next_[sender_id])) 
        {
            uint32_t next_seq = next_[sender_id];
            logger_->logDelivery(sender_id, next_seq);
            pending_[sender_id].erase(next_seq);
            next_[sender_id]++;
        }
    } else {
        // 如果是旧消息 (seq < next)，因为内存清理重入，直接丢弃
        if (seq > next_[sender_id]) {
            pending_[sender_id][seq] = {sender_id, seq};
        }
    }
}

Host FIFOBroadcastApp::findHost(uint32_t id) const 
{
    for (const Host& host : hosts_) {
        if (host.id == id) return host;
    }
    return Host();
}

uint32_t FIFOBroadcastApp::getProcessIdFromAddress(const std::string& ip, uint16_t port) const 
{
    for (const Host& host : hosts_) {
        if (host.port == port) return host.id;
    }
    return 0;
}

}