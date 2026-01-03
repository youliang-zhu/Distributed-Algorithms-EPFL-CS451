#include "perfectlink/perfect_link_app.hpp"
#include <iostream>
#include <iterator>

namespace milestone1 {

//=================Unified Sender Implementation=================

UnifiedSender::UnifiedSender(UDPSocket* socket, const std::vector<Host>& neighbors, Logger* logger)
    : socket_(socket), logger_(logger), running_(false)
{
    for (const auto& h : neighbors) {
        routing_table_[h.id] = h;
    }
}

UnifiedSender::~UnifiedSender() { stop(); }

void UnifiedSender::start() 
{
    running_ = true;
    send_thread_ = std::thread(&UnifiedSender::sendLoop, this);
    retransmit_thread_ = std::thread(&UnifiedSender::retransmitLoop, this);
}

void UnifiedSender::stop() 
{
    running_ = false;
    cv_send_.notify_all();
    cv_retransmit_.notify_all();
    if (send_thread_.joinable()) send_thread_.join();
    if (retransmit_thread_.joinable()) retransmit_thread_.join();
}

void UnifiedSender::send(uint32_t target_id, uint32_t original_sender_id, uint32_t seq_number) 
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        send_queue_.push({target_id, original_sender_id, seq_number});
    }
    cv_send_.notify_one();
}

void UnifiedSender::processAck(uint32_t source_id, const std::vector<AckItem>& acks) 
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto target_it = unacked_window_.find(source_id);
    if (target_it == unacked_window_.end()) return;

    for (const auto& ack : acks) {
        MsgKey key = {ack.sender_id, ack.seq_number};
        target_it->second.erase(key);
    }
}

void UnifiedSender::sendLoop() 
{
    while (running_) 
    {
        std::vector<PendingMessage> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_send_.wait(lock, [this] { return !send_queue_.empty() || !running_; });
            if (!running_) break;

            while (!send_queue_.empty() && batch.size() < MAX_BATCH_SIZE) 
            {
                const auto& front = send_queue_.front();
                if (!batch.empty() && (batch[0].target_id != front.target_id || 
                                       batch[0].original_sender_id != front.original_sender_id)) 
                {
                    break;
                }
                batch.push_back(front);
                send_queue_.pop();
            }
        }

        if (batch.empty()) continue;

        auto now = std::chrono::steady_clock::now();
        uint32_t target = batch[0].target_id;
        uint32_t orig = batch[0].original_sender_id;
        std::vector<uint32_t> seqs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& msg : batch) {
                seqs.push_back(msg.seq_number);
                MsgKey key = {orig, msg.seq_number};
                unacked_window_[target][key] = {orig, msg.seq_number, now};
                timeout_queue_.push({now + RETRANSMIT_TIMEOUT, target, orig, msg.seq_number});
            }
        }
        cv_retransmit_.notify_one();

        if (routing_table_.find(target) != routing_table_.end()) 
        {
            auto& host = routing_table_[target];
            Packet pkt = Packet::createDataPacket(orig, seqs);
            try {
                socket_->send(host.ip, host.port, pkt.serialize());
            } catch (...) {}
        }
    }
}

void UnifiedSender::retransmitLoop() 
{
    while (running_) 
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (timeout_queue_.empty()) {
            cv_retransmit_.wait(lock, [this] { return !timeout_queue_.empty() || !running_; });
            if (!running_) break;
        }

        auto top = timeout_queue_.top();
        auto now = std::chrono::steady_clock::now();
        
        if (top.timeout_time > now) {
            cv_retransmit_.wait_until(lock, top.timeout_time);
            if (!running_) break;
            continue;
        }

        timeout_queue_.pop();
        
        auto& window = unacked_window_[top.target_id];
        MsgKey key = {top.original_sender_id, top.seq_number};
        auto it = window.find(key);
        
        if (it != window.end()) {
            it->second.last_sent = now;
            timeout_queue_.push({now + RETRANSMIT_TIMEOUT, top.target_id, top.original_sender_id, top.seq_number});
            
            Packet pkt = Packet::createDataPacket(top.original_sender_id, {top.seq_number});
            
            if (routing_table_.find(top.target_id) != routing_table_.end()) 
            {
                auto& host = routing_table_[top.target_id];
                lock.unlock();
                try 
                {
                    socket_->send(host.ip, host.port, pkt.serialize());
                } 
                catch(...) {}
                lock.lock();
            }
        }
    }
}

//=================Receiver Implementation=================

Receiver::Receiver(UDPSocket* socket, Logger* logger) 
    : socket_(socket), logger_(logger), flush_running_(false) {}

Receiver::~Receiver() { stop(); }

void Receiver::start() 
{
    flush_running_ = true;
    flush_thread_ = std::thread(&Receiver::flushLoop, this);
}

void Receiver::stop() 
{
    flush_running_ = false;
    if (flush_thread_.joinable()) flush_thread_.join();
}

void Receiver::setMessageHandler(MessageHandler handler) { message_handler_ = handler; }

void Receiver::setAckHandler(AckHandler handler) { ack_handler_ = handler; }

void Receiver::handleData(const Packet& packet, const std::string& sender_ip, uint16_t sender_port) 
{
    std::string key = sender_ip + ":" + std::to_string(static_cast<unsigned int>(sender_port));
    
    //从P:port 解析出 udp_source_id（用于 ack_handler）
    uint32_t udp_source_id = 0;
    size_t colon = sender_ip.rfind(':');
    //简化：直接通过port查找（需要在上层设置）
    
    std::lock_guard<std::mutex> lock(mtx_);
    
    for (uint32_t seq : packet.seq_numbers) 
    {
        // 始终 ACK
        pending_acks_[key].push_back({packet.sender_id, seq});
        auto& delivered_set = delivered_[packet.sender_id];
        // 内存清理
        if (delivered_set.size() >= MAX_DELIVERED_WINDOW) 
        {
            auto it = delivered_set.begin();
            std::advance(it, MAX_DELIVERED_WINDOW / 2);
            delivered_set.erase(delivered_set.begin(), it);
        }

        bool is_new = delivered_set.insert(seq).second;
        
        // 总是调用 ack_handler（用于 URB 收集 ACK）
        if (ack_handler_) 
        {
            ack_handler_(packet.sender_id, seq, sender_ip, sender_port);
        }

        if (is_new) 
        {
            if (message_handler_) 
            {
                message_handler_(packet.sender_id, seq, sender_ip, sender_port);
            } 
            else 
            {
                // M1 逻辑：如果没有 handler，直接打日志（兼容 Milestone 1）
                logger_->logDelivery(packet.sender_id, seq);
            }
        }
    }
}

void Receiver::flushLoop() 
{
    while (flush_running_) {
        std::this_thread::sleep_for(ACK_FLUSH_TIMEOUT);
        
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [key, ack_list] : pending_acks_) {
            if (ack_list.empty()) continue;
            
            size_t colon_pos = key.find(':');
            std::string ip = key.substr(0, colon_pos);
            uint16_t port = static_cast<uint16_t>(std::stoul(key.substr(colon_pos + 1)));
            
            while (!ack_list.empty()) 
            {
                size_t batch_size = std::min(ack_list.size(), MAX_BATCH_SIZE);
                std::vector<AckItem> batch(ack_list.begin(), ack_list.begin() + batch_size);
                
                Packet ack = Packet::createAckPacket(batch);
                try 
                {
                    socket_->send(ip, port, ack.serialize());
                } 
                catch(...) {}
                
                ack_list.erase(ack_list.begin(), ack_list.begin() + batch_size);
            }
        }
    }
}

//=================PerfectLinkApp Implementation=================

PerfectLinkApp::PerfectLinkApp(uint32_t my_id, const std::vector<Host>& hosts,
                               uint32_t m, uint32_t receiver_id, const std::string& output_path)
    : my_id_(my_id), hosts_(hosts), m_(m), receiver_id_(receiver_id), running_(false)
{
    Host my_host = findHost(my_id_);
    logger_ = new Logger(output_path);
    
    // 使用新架构：Single Socket
    socket_ = new UDPSocket(my_host.port);
    
    unified_sender_ = new UnifiedSender(socket_, hosts_, logger_);
    receiver_ = new Receiver(socket_, logger_);
    // M1 不需要设置 MessageHandler，让 Receiver 默认打日志即可
}

PerfectLinkApp::~PerfectLinkApp() {
    shutdown();
    delete unified_sender_;
    delete receiver_;
    delete logger_;
    delete socket_;
}

bool PerfectLinkApp::isSender() const
{
    return my_id_ != receiver_id_;
}

void PerfectLinkApp::run() {
    running_ = true;
    receive_thread_ = std::thread(&PerfectLinkApp::receiveLoop, this);
    
    receiver_->start();
    unified_sender_->start();
    
    // 简单的冷启动同步
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 如果是 Sender，发送 M 条消息
    if (isSender()) {
        for (uint32_t seq = 1; seq <= m_; seq++) {
            // [Fix] 必须记录广播日志，否则 M1 测试失败
            logger_->logBroadcast(seq);
            
            // original_sender_id 就是 my_id_
            unified_sender_->send(receiver_id_, my_id_, seq);
        }
        
        logger_->flush();
    }
}

void PerfectLinkApp::shutdown() 
{
    running_ = false;
    if (socket_) socket_->close();
    if (unified_sender_) unified_sender_->stop();
    if (receiver_) receiver_->stop();
    if (receive_thread_.joinable()) receive_thread_.join();
    logger_->flush();
}

void PerfectLinkApp::receiveLoop() 
{
    while (running_) {
        try {
            auto [data, sender_ip, sender_port] = socket_->receive();
            uint32_t immediate_source = getProcessIdFromAddress(sender_ip, sender_port);
            Packet packet = Packet::deserialize(data);
            
            if (packet.type == MessageType::PERFECT_LINK_DATA) {
                receiver_->handleData(packet, sender_ip, sender_port);
            } 
            else if (packet.type == MessageType::PERFECT_LINK_ACK) {
                unified_sender_->processAck(immediate_source, packet.ack_payloads);
            }
        } catch (...) {
            if (!running_) break;
        }
    }
}

Host PerfectLinkApp::findHost(uint32_t id) const 
{
    for (const Host& host : hosts_) {
        if (host.id == id) return host;
    }
    return Host();
}

uint32_t PerfectLinkApp::getProcessIdFromAddress(const std::string& ip, uint16_t port) const {
    for (const Host& host : hosts_) 
    {
        if (host.port == port) return host.id;
    }
    return 0;
}

uint32_t PerfectLinkApp::getProcessIdFromAddress(const std::string& ip, uint16_t port) const {
    for (const Host& host : hosts_) {
        if (host.port == port) return host.id;
    }
    return 0;
}

}