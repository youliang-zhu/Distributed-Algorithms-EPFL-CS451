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
    
    if (!ack_seqs.empty()) {
        std::cout << "[DEBUG] handleAck: received ACK for " << ack_seqs.size() 
                  << " messages, first=" << ack_seqs.front()
                  << ", last=" << ack_seqs.back()
                  << ", unacked_before=" << unacked_messages_.size();
    }
    
    int removed_count = 0;
    for (uint32_t seq : ack_seqs) 
    {
        auto it = unacked_messages_.find(seq);
        if (it != unacked_messages_.end()) {
            unacked_messages_.erase(it);
            removed_count++;
        }
    }
    
    if (!ack_seqs.empty()) {
        std::cout << ", removed=" << removed_count
                  << ", unacked_after=" << unacked_messages_.size() << std::endl;
    }
}

bool Sender::allMessagesAcked() const 
{
    std::lock_guard<std::mutex> lock(mtx_);
    return pending_queue_.empty() && unacked_messages_.empty();
}

void Sender::waitUntilAllAcked() 
{
    int loop_count = 0;
    while (running_ && !allMessagesAcked()) 
    {
        loop_count++;
        if (loop_count % 20 == 1) {  // Print every second (20 * 50ms)
            std::lock_guard<std::mutex> lock(mtx_);
            std::cout << "[DEBUG] waitUntilAllAcked loop #" << loop_count 
                      << ": pending=" << pending_queue_.size() 
                      << ", unacked=" << unacked_messages_.size() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cout << "[DEBUG] waitUntilAllAcked exiting after " << loop_count << " iterations" << std::endl;
}

void Sender::sendLoop() 
{
    int loop_count = 0;
    while (running_) 
    {
        loop_count++;
        if (loop_count % 100 == 1) {  // 每秒打印一次 (100 * 10ms)
            std::lock_guard<std::mutex> lock(mtx_);
            std::cout << "[DEBUG] sendLoop #" << loop_count 
                      << ": pending=" << pending_queue_.size() 
                      << ", unacked=" << unacked_messages_.size() << std::endl;
        }
        sendNewMessages();
        retransmitTimedOut();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "[DEBUG] sendLoop exiting after " << loop_count << " iterations" << std::endl;
}

void Sender::sendNewMessages() 
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<uint32_t> batch;
    
    // 记录batch数量
    static int batch_count = 0;
    
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
        batch_count++;
        std::cout << "[DEBUG] sendNewMessages batch #" << batch_count 
                  << ": size=" << batch.size() 
                  << ", first_seq=" << batch.front() 
                  << ", last_seq=" << batch.back()
                  << ", remaining_pending=" << pending_queue_.size()
                  << ", total_unacked=" << unacked_messages_.size() << std::endl;
        
        // 创建一个包含多个序列号的数据包
        Packet packet = Packet::createDataPacket(my_id_, batch);
        // 序列化变为字节流
        std::vector<uint8_t> bytes = packet.serialize();
        
        std::cout << "[DEBUG]   Sending packet: " << bytes.size() << " bytes" << std::endl;
        
        // 输入目标ip，端口，数据，使用sendto发送数据，不可靠传输，立即返回结果，udp传输
        try {
            socket_->send(receiver_.ip, receiver_.port, bytes);
            std::cout << "[DEBUG]   ✓ Send successful" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[DEBUG]   ✗ Send FAILED: " << e.what() << std::endl;
        }
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
        std::cout << "[DEBUG] retransmitTimedOut: retransmitting " << to_retransmit.size() 
                  << " messages, first_seq=" << to_retransmit.front()
                  << ", last_seq=" << to_retransmit.back() << std::endl;
        
        Packet packet = Packet::createDataPacket(my_id_, to_retransmit);
        std::vector<uint8_t> bytes = packet.serialize();
        
        try {
            socket_->send(receiver_.ip, receiver_.port, bytes);
            std::cout << "[DEBUG]   Retransmit sent: " << bytes.size() << " bytes" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[DEBUG]   Retransmit FAILED: " << e.what() << std::endl;
        }
    }
}

// ============================================================================
// Receiver Implementation
// ============================================================================

Receiver::Receiver(UDPSocket* socket, Logger* logger)
    : socket_(socket), logger_(logger), flush_running_(false) {
}

Receiver::~Receiver() 
{
    stop();
}

void Receiver::start() 
{
    flush_running_ = true;
    flush_thread_ = std::thread(&Receiver::flushLoop, this);
}

void Receiver::stop() 
{
    flush_running_ = false;
    if (flush_thread_.joinable()) 
    {
        flush_thread_.join();
    }
}

void Receiver::handle(const Packet& packet, const std::string& sender_ip, uint16_t sender_port) 
{
    if (packet.type != MessageType::PERFECT_LINK_DATA) 
    {
        return;
    }
    
    static unsigned int packet_count = 0;
    static std::unordered_map<uint32_t, unsigned int> delivery_count_per_sender;
    
    packet_count++;
    uint32_t sender_id = packet.sender_id;
    
    unsigned int new_deliveries = 0;
    bool should_flush = false;
    
    {
        // 锁定的局部作用域开始
        std::lock_guard<std::mutex> lock(mtx_);
        for (uint32_t seq : packet.seq_numbers) 
        {
            std::unordered_set<uint32_t>& delivered = delivered_messages_[sender_id];
             // 使用O(1)哈希查找
            if (delivered.find(seq) == delivered.end()) 
            {
                logger_->logDelivery(sender_id, seq);
                delivered.insert(seq);
                new_deliveries++;
                delivery_count_per_sender[sender_id]++;
            }
        }
        
        if (packet_count % 50 == 1 || new_deliveries > 0) {
            std::cout << "[DEBUG] Receiver::handle packet #" << packet_count 
                      << " from sender " << sender_id
                      << ": received " << packet.seq_numbers.size() << " seqs"
                      << ", new_deliveries=" << new_deliveries
                      << ", total_from_sender=" << delivery_count_per_sender[sender_id] << std::endl;
        }

        // 创建一个sender字符串键，用于标识消息应该回复给谁, 将所有收到的序号加入待发送 ACK 队列
        std::string key = sender_ip + ":" + std::to_string(static_cast<unsigned int>(sender_port));
        for (uint32_t seq : packet.seq_numbers) 
        {
            pending_acks_[key].push_back(seq);
        }

        //检查是否需要flush，但不在锁内调用
        if (pending_acks_[key].size() >= MAX_ACKS_PER_PACKET) 
        {
            std::cout << "[DEBUG]   Triggering immediate ACK flush (threshold reached)" << std::endl;
            should_flush = true;
        }
    }
    
    // !!!critical change
    //在锁外调用flushAcks，避免死锁
    if (should_flush) 
    {
        flushAcks(sender_ip, sender_port);
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
    
    size_t total_acks_sent = 0;
    while (!pending_acks_[key].empty()) 
    {
        size_t batch_size = std::min(pending_acks_[key].size(), MAX_ACKS_PER_PACKET);
        std::vector<uint32_t> batch(pending_acks_[key].begin(), 
                                     pending_acks_[key].begin() + batch_size);
        
        Packet ack = Packet::createAckPacket(batch);
        std::vector<uint8_t> ack_bytes = ack.serialize();
        
        try {
            socket_->send(sender_ip, sender_port, ack_bytes);
            total_acks_sent += batch_size;
        } catch (const std::exception& e) {
            std::cerr << "[DEBUG] ACK send FAILED: " << e.what() << std::endl;
        }
        
        pending_acks_[key].erase(pending_acks_[key].begin(), 
                                  pending_acks_[key].begin() + batch_size);
    }
    
    if (total_acks_sent > 0) {
        std::cout << "[DEBUG] flushAcks: sent " << total_acks_sent << " ACKs to " << sender_ip << std::endl;
    }
}

void Receiver::flushLoop() 
{
    while (flush_running_) 
    {
        // 定期检查并发送所有pending ACKs
        {
            std::lock_guard<std::mutex> lock(mtx_);
            
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
        }
        
        // 等待一段时间再继续
        std::this_thread::sleep_for(ACK_FLUSH_INTERVAL);
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

    // 启动Receiver的定期flush线程
    receiver_->start();
    
    if (sender_ != nullptr) 
    {
        sender_->start();
        for (uint32_t seq = 1; seq <= m_; seq++) 
        {
            sender_->send(seq);
        }
        logger_->flush();
        sender_->waitUntilAllAcked();
        std::cout << "Process " << my_id_ << ": Sent " << m_ << " messages to process " << receiver_id_ << std::endl;
    } 
    else 
    {
        std::cout << "Process " << my_id_ << ": Ready to receive messages" << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void PerfectLinkApp::shutdown() 
{
    // 停止 Receiver 的 flushLoop（周期性主动发送 ACK）
    if (receiver_ != nullptr) 
    {
        receiver_->stop();
    }
    
    // 停止 Sender（不再发送数据包）
    if (sender_ != nullptr) 
    {
        sender_->stop();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running_ = false;
    if (receive_thread_.joinable()) 
    {
        // !!!critical change
        receive_thread_.detach();
    }
    
    // 最后 flush logger（只写文件，不发网络包）
    logger_->flush();
}


void PerfectLinkApp::receiveLoop() 
{
    std::cout << "[DEBUG] receiveLoop started" << std::endl;
    int packets_received = 0;
    int data_packets = 0;
    int ack_packets = 0;
    
    while (running_) 
    {
        try 
        {
            // 接收数据包并反序列化，auto 自动推导类型并解包tuple，receive返回值是一个tuple
            auto [data, sender_ip, sender_port] = socket_->receive();
            packets_received++;
            
            // 仅在receiveLoop中使用反序列化，把多个数据包的字节流恢复为Packet结构
            Packet packet = Packet::deserialize(data);
            if (packet.type == MessageType::PERFECT_LINK_DATA) 
            {
                data_packets++;
                receiver_->handle(packet, sender_ip, sender_port);
            } 
            else if (packet.type == MessageType::PERFECT_LINK_ACK) 
            {
                ack_packets++;
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
                std::cerr << "[DEBUG] Error in receiveLoop: " << e.what() << std::endl;
            }
            break;
        }
    }
    
    std::cout << "[DEBUG] receiveLoop exiting: total_packets=" << packets_received 
              << ", data=" << data_packets 
              << ", ack=" << ack_packets << std::endl;
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