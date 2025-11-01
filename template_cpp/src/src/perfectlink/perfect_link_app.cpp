#include "perfectlink/perfect_link_app.hpp"
#include <iostream> 
#include <algorithm>

namespace milestone1 {

// ============================================================================
// Sender Implementation
// ============================================================================

Sender::Sender(UDPSocket* socket, uint32_t my_id, const Host& receiver, Logger* logger)
    : socket_(socket), my_id_(my_id), receiver_(receiver), logger_(logger), running_(false) {
}

Sender::~Sender() 
{
    stop();
}

void Sender::start() 
{
    running_ = true;
    send_thread_ = std::thread(&Sender::sendLoop, this);
}

void Sender::stop() 
{
    running_ = false;
    if (send_thread_.joinable()) 
    {
        send_thread_.join();
    }
}

void Sender::send(uint32_t seq_number) 
{
    std::lock_guard<std::mutex> lock(mtx_);
    pending_queue_.push(seq_number);
    logger_->logBroadcast(seq_number);
}

void Sender::handleAck(const std::vector<uint32_t>& ack_seqs) 
{
    std::lock_guard<std::mutex> lock(mtx_);
    for (uint32_t seq : ack_seqs) 
    {
        unacked_messages_.erase(seq);
    }
}

void Sender::sendLoop() 
{
    while (running_) 
    {
        sendNewMessages();
        retransmitTimedOut();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Sender::sendNewMessages() 
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<uint32_t> batch;
    while (!pending_queue_.empty() && batch.size() < MAX_BATCH_SIZE) 
    {
        uint32_t seq = pending_queue_.front();
        pending_queue_.pop();
        
        batch.push_back(seq);
        
        // Add to unacked messages
        unacked_messages_[seq] = SentMessage(seq, std::chrono::steady_clock::now());
    }
    
    if (!batch.empty()) 
    {
        // 创建一个包含多个序列号的数据包
        Packet packet = Packet::createDataPacket(my_id_, batch);
        // 序列化变为字节流
        std::vector<uint8_t> bytes = packet.serialize();
        // 输入目标ip，端口，数据，使用sendto发送数据，不可靠传输，立即返回结果，udp传输
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
                std::cout << "NEW: d " << sender_id << " " << seq << std::endl;
            }
            else 
            {
                std::cout << "DUP: d " << sender_id << " " << seq << " (already delivered)" << std::endl;
            }
        }

        // 创建一个sender字符串键，用于标识消息应该回复给谁, 将所有收到的序号加入待发送 ACK 队列
        std::string key = sender_ip + ":" + std::to_string(static_cast<unsigned int>(sender_port));
        for (uint32_t seq : packet.seq_numbers) 
        {
            pending_acks_[key].push_back(seq);
        }
    }
    
    auto now = std::chrono::steady_clock::now();
    std::string key = sender_ip + ":" + std::to_string(static_cast<unsigned int>(sender_port));
    if (now - last_ack_time_ > ACK_BATCH_INTERVAL || pending_acks_[key].size() >= MAX_ACKS_PER_PACKET) 
    {
        flushAcks(sender_ip, sender_port);
        last_ack_time_ = now;
    }
}

void Receiver::flushAcks(const std::string& sender_ip, uint16_t sender_port) 
{
    std::string key = sender_ip + ":" + std::to_string(static_cast<unsigned int>(sender_port));
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

void Receiver::flushAllPendingAcks() 
{
    std::lock_guard<std::mutex> lock(mtx_);
    
    // 遍历所有pending ACKs并发送
    for (auto& [key, ack_list] : pending_acks_) 
    {
        if (ack_list.empty()) 
        {
            continue;
        }
        
        // 解析key获取IP和port
        size_t colon_pos = key.find(':');
        std::string sender_ip = key.substr(0, colon_pos);
        uint16_t sender_port = static_cast<uint16_t>(
            std::stoul(key.substr(colon_pos + 1))
        );
        
        // 发送所有pending ACKs
        while (!ack_list.empty()) 
        {
            size_t batch_size = std::min(ack_list.size(), MAX_ACKS_PER_PACKET);
            std::vector<uint32_t> batch(ack_list.begin(), ack_list.begin() + batch_size);
            Packet ack = Packet::createAckPacket(batch);
            std::vector<uint8_t> ack_bytes = ack.serialize();
            socket_->send(sender_ip, sender_port, ack_bytes);
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
    : my_id_(my_id), hosts_(hosts), m_(m), receiver_id_(receiver_id), running_(false) 
{
    Host my_host = findHost(my_id_);
    socket_ = new UDPSocket(my_host.port);
    logger_ = new Logger(output_path);
    
    //create sender and receiver
    if (my_id_ != receiver_id_) 
    {
        // I am a sender
        Host receiver_host = findHost(receiver_id_);
        sender_ = new Sender(socket_, my_id_, receiver_host, logger_);
    } 
    else 
    {
        sender_ = nullptr;
    }
    
    receiver_ = new Receiver(socket_, logger_);
}

PerfectLinkApp::~PerfectLinkApp() 
{
    shutdown();
    
    delete receiver_;
    delete sender_;
    delete logger_;
    delete socket_;
}

void PerfectLinkApp::run() 
{
    running_ = true;
    receive_thread_ = std::thread(&PerfectLinkApp::receiveLoop, this);
    
    if (sender_ != nullptr) 
    {
        sender_->start();
        for (uint32_t seq = 1; seq <= m_; seq++) 
        {
            sender_->send(seq);
        }
        std::cout << "Process " << my_id_ << ": Sent " << m_ << " messages to process " << receiver_id_ << std::endl;
    } 
    else 
    {
        std::cout << "Process " << my_id_ << ": Ready to receive messages" << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // 直接返回，不等待
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
    receiver_->flushAllPendingAcks();
    logger_->flush();
}

void PerfectLinkApp::receiveLoop() 
{
    while (running_) 
    {
        try 
        {
            // 接收数据包并反序列化，auto 自动推导类型并解包tuple，receive返回值是一个tuple
            auto [data, sender_ip, sender_port] = socket_->receive();
            // 仅在receiveLoop中使用反序列化，把多个数据包的字节流恢复为Packet结构
            Packet packet = Packet::deserialize(data);
            if (packet.type == MessageType::PERFECT_LINK_DATA) 
            {
                receiver_->handle(packet, sender_ip, sender_port);
            } 
            else if (packet.type == MessageType::PERFECT_LINK_ACK) 
            {
                if (sender_ != nullptr) 
                {
                    sender_->handleAck(packet.seq_numbers);
                }
            }
        } 
        catch (const std::exception& e) 
        {
            // Socket might throw when stopping
            if (running_) 
            {
                // Only print error if we're still supposed to be running
                // Otherwise it's expected (socket closed during shutdown)
            }
            break;
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