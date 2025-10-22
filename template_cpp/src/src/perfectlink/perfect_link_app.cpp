#include "perfect_link_app.hpp"
#include <algorithm>

namespace milestone1 {

// ============================================================================
// Sender Implementation
// ============================================================================

Sender::Sender(UDPSocket* socket, uint32_t my_id, const Host& receiver, Logger* logger)
    : socket_(socket), my_id_(my_id), receiver_(receiver), logger_(logger), running_(false) {
}

Sender::~Sender() {
    stop();
}

void Sender::start() {
    running_ = true;
    send_thread_ = std::thread(&Sender::sendLoop, this);
}

void Sender::stop() {
    running_ = false;
    if (send_thread_.joinable()) {
        send_thread_.join();
    }
}

void Sender::send(uint32_t seq_number) {
    std::lock_guard<std::mutex> lock(mtx_);
    pending_queue_.push(seq_number);
    logger_->logBroadcast(seq_number);
}

void Sender::handleAck(const std::vector<uint32_t>& ack_seqs) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (uint32_t seq : ack_seqs) {
        unacked_messages_.erase(seq);
    }
}

void Sender::sendLoop() {
    while (running_) {
        sendNewMessages();
        retransmitTimedOut();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Sender::sendNewMessages() {
    std::lock_guard<std::mutex> lock(mtx_);
    
    std::vector<uint32_t> batch;
    while (!pending_queue_.empty() && batch.size() < MAX_BATCH_SIZE) {
        uint32_t seq = pending_queue_.front();
        pending_queue_.pop();
        
        batch.push_back(seq);
        
        // Add to unacked messages
        unacked_messages_[seq] = SentMessage(seq, std::chrono::steady_clock::now());
    }
    
    if (!batch.empty()) {
        Packet packet = Packet::createDataPacket(my_id_, batch);
        std::vector<uint8_t> bytes = packet.serialize();
        socket_->send(receiver_.ip, receiver_.port, bytes);
    }
}

void Sender::retransmitTimedOut() {
    std::lock_guard<std::mutex> lock(mtx_);
    
    auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> to_retransmit;
    
    for (auto& [seq, sent_msg] : unacked_messages_) {
        if (now - sent_msg.last_sent > TIMEOUT) {
            to_retransmit.push_back(seq);
            sent_msg.last_sent = now;
            sent_msg.retransmit_count++;
            
            if (to_retransmit.size() >= MAX_BATCH_SIZE) {
                break;
            }
        }
    }
    
    if (!to_retransmit.empty()) {
        Packet packet = Packet::createDataPacket(my_id_, to_retransmit);
        std::vector<uint8_t> bytes = packet.serialize();
        socket_->send(receiver_.ip, receiver_.port, bytes);
    }
}

// ============================================================================
// Receiver Implementation
// ============================================================================

Receiver::Receiver(UDPSocket* socket, Logger* logger)
    : socket_(socket), logger_(logger), last_ack_time_(std::chrono::steady_clock::now()) {
}

void Receiver::handle(const Packet& packet, const std::string& sender_ip, uint16_t sender_port) 
{
    if (packet.type != MessageType::PERFECT_LINK_DATA) 
    {
        return;
    }
    
    uint32_t sender_id = packet.sender_id;
    std::vector<uint32_t> new_deliveries;
    
    {
        // 锁定的局部作用域开始
        std::lock_guard<std::mutex> lock(mtx_);
        
        for (uint32_t seq : packet.seq_numbers) 
        {
            auto& delivered = delivered_messages_[sender_id];
            // maybe slow problem!!!!
            bool already_delivered = std::find(delivered.begin(), delivered.end(), seq) != delivered.end();
            
            if (!already_delivered) 
            {
                logger_->logDelivery(sender_id, seq);
                delivered.push_back(seq);
                if (delivered.size() > MAX_DELIVERED_WINDOW) 
                {
                    delivered.pop_front();
                }
            }
        }

        // 创建一个sender字符串键，用于标识消息应该回复给谁, 将所有收到的序号加入待发送 ACK 队列
        std::string key = sender_ip + ":" + std::to_string(sender_port);
        for (uint32_t seq : packet.seq_numbers) 
        {
            pending_acks_[key].push_back(seq);
        }
    }
    
    auto now = std::chrono::steady_clock::now();
    std::string key = sender_ip + ":" + std::to_string(sender_port);
    if (now - last_ack_time_ > ACK_BATCH_INTERVAL || pending_acks_[key].size() >= MAX_ACKS_PER_PACKET) 
    {
        flushAcks(sender_ip, sender_port);
        last_ack_time_ = now;
    }
}

void Receiver::flushAcks(const std::string& sender_ip, uint16_t sender_port) {
    std::string key = sender_ip + ":" + std::to_string(sender_port);
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (pending_acks_[key].empty()) 
    {
        return;
    }
    
    while (!pending_acks_[key].empty()) 
    {
        size_t batch_size = std::min(pending_acks_[key].size(), MAX_ACKS_PER_PACKET);
        std::vector<uint32_t> batch(pending_acks_[key].begin(), 
                                     pending_acks_[key].begin() + batch_size);
        
        Packet ack = Packet::createAckPacket(batch);
        std::vector<uint8_t> ack_bytes = ack.serialize();
        socket_->send(sender_ip, sender_port, ack_bytes);
        
        pending_acks_[key].erase(pending_acks_[key].begin(), 
                                  pending_acks_[key].begin() + batch_size);
    }
}

// ============================================================================
// PerfectLinkApp Implementation
// ============================================================================

PerfectLinkApp::PerfectLinkApp(uint32_t my_id, const std::vector<Host>& hosts,
                               uint32_t m, uint32_t receiver_id, const std::string& output_path)
    : my_id_(my_id), hosts_(hosts), m_(m), receiver_id_(receiver_id), running_(false) {
    
    // Find my port
    Host my_host = findHost(my_id_);
    socket_ = new UDPSocket(my_host.port);
    
    // Create logger
    logger_ = new Logger(output_path);
    
    // Create sender and receiver
    if (my_id_ != receiver_id_) {
        // I am a sender
        Host receiver_host = findHost(receiver_id_);
        sender_ = new Sender(socket_, my_id_, receiver_host, logger_);
    } else {
        sender_ = nullptr;
    }
    
    receiver_ = new Receiver(socket_, logger_);
}

PerfectLinkApp::~PerfectLinkApp() {
    shutdown();
    
    delete receiver_;
    delete sender_;
    delete logger_;
    delete socket_;
}

void PerfectLinkApp::run() {
    running_ = true;
    
    // Start receive thread (all processes need this)
    receive_thread_ = std::thread(&PerfectLinkApp::receiveLoop, this);
    
    // If I am a sender, start sending
    if (sender_ != nullptr) {
        sender_->start();
        
        // Generate and send m messages
        for (uint32_t seq = 1; seq <= m_; seq++) {
            sender_->send(seq);
        }
    }
}

void PerfectLinkApp::shutdown() 
{
    running_ = false;
    if (sender_ != nullptr) 
    {
        sender_->stop();
    }
    if (receive_thread_.joinable()) 
    {
        receive_thread_.join();
    }
    logger_->flush();
}

void PerfectLinkApp::receiveLoop() {
    while (running_) {
        try {
            auto [data, sender_ip, sender_port] = socket_->receive();
            
            Packet packet = Packet::deserialize(data);
            
            if (packet.type == MessageType::PERFECT_LINK_DATA) {
                // Data packet -> handle by receiver
                receiver_->handle(packet, sender_ip, sender_port);
            } else if (packet.type == MessageType::PERFECT_LINK_ACK) {
                // ACK packet -> handle by sender
                if (sender_ != nullptr) {
                    sender_->handleAck(packet.seq_numbers);
                }
            }
        } catch (const std::exception& e) {
            // Socket might throw when stopping
            if (running_) {
                // Only print error if we're still supposed to be running
                // Otherwise it's expected (socket closed during shutdown)
            }
            break;
        }
    }
}

Host PerfectLinkApp::findHost(uint32_t id) const {
    for (const Host& host : hosts_) {
        if (host.id == id) {
            return host;
        }
    }
    // Should never happen if hosts file is correct
    return Host();
}

} // namespace milestone1