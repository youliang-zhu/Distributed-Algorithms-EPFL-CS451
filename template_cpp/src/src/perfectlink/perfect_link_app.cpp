#include "perfectlink/perfect_link_app.hpp"
#include <iostream> 
#include <algorithm>
#include <type_traits>
#include <unordered_map>


namespace milestone1 {

// ============================================================================
// Sender Implementation
// ============================================================================

Sender::Sender(UDPSocket* socket, uint32_t my_id, const Host& receiver, Logger* logger)
    : socket_(socket), my_id_(my_id), receiver_(receiver), logger_(logger), running_(false) {}

Sender::~Sender() {
    stop();
}

void Sender::start() {
    running_ = true;
    ack_receive_thread_ = std::thread(&Sender::ackReceiveLoop, this);
    retransmit_thread_ = std::thread(&Sender::retransmitLoop, this);
    send_thread_ = std::thread(&Sender::sendLoop, this);
}

void Sender::stop() {
    running_ = false;
    queue_cv_.notify_all();
    timeout_cv_.notify_all();
    
    if (ack_receive_thread_.joinable()) ack_receive_thread_.join();
    if (retransmit_thread_.joinable()) retransmit_thread_.join();
    if (send_thread_.joinable()) send_thread_.join();
}

void Sender::send(uint32_t seq_number) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending_queue_.push(seq_number);
        logger_->logBroadcast(seq_number);
    }
    queue_cv_.notify_one();
}

bool Sender::allMessagesAcked() const {
    std::lock_guard<std::mutex> lock1(queue_mutex_);
    std::lock_guard<std::mutex> lock2(data_mutex_);
    return pending_queue_.empty() && unacked_messages_.empty();
}

void Sender::waitUntilAllAcked() {
    while (running_ && !allMessagesAcked()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void Sender::sendLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !pending_queue_.empty() || !running_; });
        
        if (!running_) break;
        
        std::vector<uint32_t> batch;
        while (!pending_queue_.empty() && batch.size() < MAX_BATCH_SIZE) {
            batch.push_back(pending_queue_.front());
            pending_queue_.pop();
        }
        lock.unlock();
        
        if (batch.empty()) continue;
        
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> data_lock(data_mutex_);
            for (uint32_t seq : batch) {
                unacked_messages_[seq] = SentMessage(seq, now);
                timeout_queue_.push({now + TIMEOUT, seq});
            }
        }
        timeout_cv_.notify_one();
        
        Packet packet = Packet::createDataPacket(my_id_, batch);
        socket_->send(receiver_.ip, receiver_.port, packet.serialize());
    }
}

void Sender::retransmitLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(data_mutex_);
        
        if (timeout_queue_.empty()) {
            timeout_cv_.wait(lock, [this] { return !timeout_queue_.empty() || !running_; });
            if (!running_) break;
            continue;
        }
        
        auto entry = timeout_queue_.top();
        auto wait_result = timeout_cv_.wait_until(lock, entry.timeout_time);
        
        if (!running_) break;
        
        if (wait_result == std::cv_status::timeout) {
            auto now = std::chrono::steady_clock::now();
            std::vector<uint32_t> to_retransmit;
            
            while (!timeout_queue_.empty() && to_retransmit.size() < MAX_BATCH_SIZE) {
                auto e = timeout_queue_.top();
                if (e.timeout_time > now) break;
                
                timeout_queue_.pop();
                
                auto it = unacked_messages_.find(e.seq_number);
                if (it == unacked_messages_.end()) continue;
                
                to_retransmit.push_back(e.seq_number);
                it->second.last_sent = now;
                it->second.retransmit_count++;
                timeout_queue_.push({now + TIMEOUT, e.seq_number});
            }
            
            if (!to_retransmit.empty()) {
                lock.unlock();
                Packet packet = Packet::createDataPacket(my_id_, to_retransmit);
                socket_->send(receiver_.ip, receiver_.port, packet.serialize());
            }
        }
    }
}

void Sender::ackReceiveLoop() {
    while (running_) {
        try {
            auto [data, sender_ip, sender_port] = socket_->receive();
            Packet packet = Packet::deserialize(data);
            
            if (packet.type == MessageType::PERFECT_LINK_ACK) {
                std::lock_guard<std::mutex> lock(data_mutex_);
                for (uint32_t seq : packet.seq_numbers) {
                    unacked_messages_.erase(seq);
                }
                timeout_cv_.notify_one();
            }
        } catch (const std::exception&) {
            if (!running_) break;
        }
    }
}

// ============================================================================
// Receiver Implementation
// ============================================================================

Receiver::Receiver(UDPSocket* socket, Logger* logger)
    : socket_(socket), logger_(logger), flush_running_(false) {}

Receiver::~Receiver() {
    stop();
}

void Receiver::start() {
    flush_running_ = true;
    flush_thread_ = std::thread(&Receiver::flushLoop, this);
}

void Receiver::stop() {
    flush_running_ = false;
    if (flush_thread_.joinable()) flush_thread_.join();
}

void Receiver::handle(const Packet& packet, const std::string& sender_ip, uint16_t sender_port) {
    if (packet.type != MessageType::PERFECT_LINK_DATA) return;
    
    std::string key = sender_ip + ":" + std::to_string(static_cast<unsigned int>(sender_port));
    std::lock_guard<std::mutex> lock(mtx_);
    
    uint32_t sender_id = packet.sender_id;
    std::set<uint32_t>& delivered = delivered_messages_[sender_id];
    
    if (delivered.size() >= MAX_DELIVERED_WINDOW) {
        delivered.erase(delivered.begin());
    }
    
    for (uint32_t seq : packet.seq_numbers) {
        if (delivered.find(seq) == delivered.end()) {
            logger_->logDelivery(sender_id, seq);
            delivered.insert(seq);
        }
        pending_acks_[key].push_back(seq);
    }
    
    if (pending_acks_[key].size() >= ACK_BATCH_SIZE) {
        std::vector<uint32_t> batch(pending_acks_[key].begin(), 
                                     pending_acks_[key].begin() + ACK_BATCH_SIZE);
        Packet ack = Packet::createAckPacket(batch);
        socket_->send(sender_ip, sender_port, ack.serialize());
        pending_acks_[key].erase(pending_acks_[key].begin(), 
                                  pending_acks_[key].begin() + ACK_BATCH_SIZE);
    }
}

void Receiver::flushLoop() {
    while (flush_running_) {
        std::this_thread::sleep_for(ACK_FLUSH_TIMEOUT);
        
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [key, ack_list] : pending_acks_) {
            if (ack_list.empty()) continue;
            
            size_t colon_pos = key.find(':');
            std::string sender_ip = key.substr(0, colon_pos);
            uint16_t sender_port = static_cast<uint16_t>(std::stoul(key.substr(colon_pos + 1)));
            
            while (!ack_list.empty()) {
                size_t batch_size = std::min(ack_list.size(), static_cast<size_t>(ACK_BATCH_SIZE));
                std::vector<uint32_t> batch(ack_list.begin(), ack_list.begin() + batch_size);
                Packet ack = Packet::createAckPacket(batch);
                socket_->send(sender_ip, sender_port, ack.serialize());
                ack_list.erase(ack_list.begin(), ack_list.begin() + batch_size);
            }
        }
    }
}

void Receiver::flushAllPendingAcks() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& [key, ack_list] : pending_acks_) {
        if (ack_list.empty()) continue;
        
        size_t colon_pos = key.find(':');
        std::string sender_ip = key.substr(0, colon_pos);
        uint16_t sender_port = static_cast<uint16_t>(std::stoul(key.substr(colon_pos + 1)));
        
        while (!ack_list.empty()) {
            size_t batch_size = std::min(ack_list.size(), static_cast<size_t>(ACK_BATCH_SIZE));
            std::vector<uint32_t> batch(ack_list.begin(), ack_list.begin() + batch_size);
            Packet ack = Packet::createAckPacket(batch);
            socket_->send(sender_ip, sender_port, ack.serialize());
            ack_list.erase(ack_list.begin(), ack_list.begin() + batch_size);
        }
    }
    pending_acks_.clear();
}

// ============================================================================
// PerfectLinkApp Implementation
// ============================================================================

PerfectLinkApp::PerfectLinkApp(uint32_t my_id, const std::vector<Host>& hosts,
                               uint32_t m, uint32_t receiver_id, const std::string& output_path)
    : my_id_(my_id), hosts_(hosts), m_(m), receiver_id_(receiver_id), running_(false) {
    std::cout << "[DEBUG] PerfectLinkApp constructor: id=" << my_id 
              << ", m=" << m << ", receiver=" << receiver_id 
              << ", output=" << output_path << std::endl;
    
    Host my_host = findHost(my_id_);
    std::cout << "[DEBUG] My host: id=" << my_host.id << ", port=" << my_host.port << std::endl;
    
    receiver_socket_ = new UDPSocket(my_host.port);
    sender_socket_ = new UDPSocket(static_cast<uint16_t>(my_host.port + 1000));
    std::cout << "[DEBUG] Sockets created: receiver_port=" << my_host.port 
              << ", sender_port=" << (my_host.port + 1000) << std::endl;
    
    logger_ = new Logger(output_path);
    
    if (my_id_ != receiver_id_) {
        std::cout << "[DEBUG] I am SENDER" << std::endl;
        Host receiver_host = findHost(receiver_id_);
        sender_ = new Sender(sender_socket_, my_id_, receiver_host, logger_);
    } else {
        std::cout << "[DEBUG] I am RECEIVER" << std::endl;
        sender_ = nullptr;
    }
    
    receiver_ = new Receiver(receiver_socket_, logger_);
    std::cout << "[DEBUG] PerfectLinkApp constructor complete" << std::endl;
}

PerfectLinkApp::~PerfectLinkApp() {
    shutdown();
    delete receiver_;
    delete sender_;
    delete logger_;
    delete sender_socket_;
    delete receiver_socket_;
}

void PerfectLinkApp::run() {
    std::cout << "[DEBUG] PerfectLinkApp::run() started" << std::endl;
    
    running_ = true;
    receive_thread_ = std::thread(&PerfectLinkApp::receiveLoop, this);
    receiver_->start();
    
    if (sender_ != nullptr) {
        std::cout << "[DEBUG] Starting sender, will send " << m_ << " messages" << std::endl;
        sender_->start();
        
        for (uint32_t seq = 1; seq <= m_; seq++) {
            sender_->send(seq);
        }
        std::cout << "[DEBUG] All messages queued, flushing logger" << std::endl;
        logger_->flush();
        
        std::cout << "[DEBUG] Waiting for all ACKs..." << std::endl;
        sender_->waitUntilAllAcked();
        std::cout << "[DEBUG] All messages acked!" << std::endl;
    } else {
        std::cout << "[DEBUG] Receiver mode, listening for messages" << std::endl;
    }
    
    std::cout << "[DEBUG] PerfectLinkApp::run() complete" << std::endl;
}

void PerfectLinkApp::shutdown() {
    std::cout << "[DEBUG] PerfectLinkApp::shutdown() started" << std::endl;
    
    receiver_->stop();
    if (sender_ != nullptr) sender_->stop();
    
    running_ = false;
    if (receive_thread_.joinable()) receive_thread_.detach();
    
    std::cout << "[DEBUG] Flushing logger before exit" << std::endl;
    logger_->flush();
    std::cout << "[DEBUG] PerfectLinkApp::shutdown() complete" << std::endl;
}


void PerfectLinkApp::receiveLoop() {
    while (running_) {
        try {
            auto [data, sender_ip, sender_port] = receiver_socket_->receive();
            Packet packet = Packet::deserialize(data);
            if (packet.type == MessageType::PERFECT_LINK_DATA) {
                receiver_->handle(packet, sender_ip, sender_port);
            }
        } catch (const std::exception&) {
            if (!running_) break;
        }
    }
}

Host PerfectLinkApp::findHost(uint32_t id) const 
{
    for (const Host& host : hosts_) 
    {
        if (host.id == id) 
        {
            return host;
        }
    }
    return Host();
}

}