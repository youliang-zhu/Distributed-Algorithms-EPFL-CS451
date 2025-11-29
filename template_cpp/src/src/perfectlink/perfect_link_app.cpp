#include "perfectlink/perfect_link_app.hpp"
#include <iostream> 
#include <algorithm>
#include <type_traits>
#include <unordered_map>


namespace milestone1 
{

// ======================
// Sender 
// ======================

Sender::Sender(UDPSocket* socket, uint32_t my_id, const Host& receiver, Logger* logger)
    : socket_(socket), my_id_(my_id), receiver_(receiver), logger_(logger), running_(false) {}

Sender::~Sender() 
{
    stop();
}

void Sender::start() 
{
    running_ = true;
    //线程3：sender发送数据包
    send_thread_ = std::thread(&Sender::sendLoop, this);
    //线程5：sender接收ACK包
    ack_receive_thread_ = std::thread(&Sender::ackReceiveLoop, this);
    //线程4: sender重传数据包
    retransmit_thread_ = std::thread(&Sender::retransmitLoop, this);
}

void Sender::stop() 
{
    running_ = false;
    //强制唤醒休眠的sendLoop()线程，让该线程可以检查到 running_ = false 条件，从而安全地退出它的主循环。
    queue_cv_.notify_all();
    //确保retransmitLoop()线程不会因为等待超时而永远阻塞，唤醒它以便它能检查running_标志并退出
    timeout_cv_.notify_all();
    
    //ackReceiveLoop线程在socket_->receive()阻塞等待数据，deatch强制中断退出（通过关闭socket）
    if (ack_receive_thread_.joinable()) ack_receive_thread_.detach();

    //retransmitLoop在timeout_cv_.wait_until被唤醒后会检查running_退出循环，所以用join等它处理完再退出更安全。
    if (retransmit_thread_.joinable()) retransmit_thread_.join();

    //sendloop在queue_cv_.wait()被唤醒后会检查running_退出循环。所以用join等它们处理完再退出更安全。
    if (send_thread_.joinable()) send_thread_.join();
}

void Sender::send(uint32_t seq_number) 
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending_queue_.push(seq_number);
        logger_->logBroadcast(seq_number);
    }
    //当外部调用send, 向pending_queue_ 成功加入一个新消息后，它调用 notify_one(), 
    //如果此时 sendLoop() 线程正在 wait 上休眠，它会被唤醒，重新获取锁，检查队列非空的条件，然后开始发送数据。
    queue_cv_.notify_one();
}

bool Sender::allMessagesAcked() const 
{
    std::lock_guard<std::mutex> lock1(queue_mutex_);
    std::lock_guard<std::mutex> lock2(data_mutex_);
    return pending_queue_.empty() && unacked_messages_.empty();
}

void Sender::waitUntilAllAcked() 
{
    int wait_count = 0;
    while (running_ && !allMessagesAcked()) 
    {
        if (wait_count % 20 == 0) 
        {  // Print every 1 second
            std::lock_guard<std::mutex> lock1(queue_mutex_);
            std::lock_guard<std::mutex> lock2(data_mutex_);
        }
        wait_count++;
    }
}

void Sender::sendLoop() 
{
    while (running_) 
    {
        //sendloop线程获取queue_mutex_锁
        std::unique_lock<std::mutex> lock(queue_mutex_);
        //如果 pending_queue_ 不为空或 running_ 为 false，线程立即退出等待，开始处理数据。
        //如果 pending_queue_ 为空，线程释放锁，进入休眠/阻塞状态，直到被通知 (notify)
        queue_cv_.wait(lock, [this] { return !pending_queue_.empty() || !running_; });
        
        if (!running_) break;
        
        std::vector<uint32_t> batch;
        //当前pending_queue_不为空，也就是有东西要send，且batch未满时，一次性取出多个消息进行发送
        while (!pending_queue_.empty() && batch.size() < MAX_BATCH_SIZE)
        {
            batch.push_back(pending_queue_.front());
            pending_queue_.pop();
        }
        lock.unlock();
        
        if (batch.empty()) continue;
        
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> data_lock(data_mutex_);
            for (uint32_t seq : batch) 
            {
                //把batch中的每个消息都加入到unacked_messages_和timeout_queue_中
                unacked_messages_[seq] = SentMessage(seq, now);
                timeout_queue_.push({now + TIMEOUT, seq});
            }
        }
        //新消息的超时信息添加到timeout_queue_中。如果此时 retransmitLoop() 正处于无限等待（因为之前队列为空），
        //这个 notify_one() 会唤醒它，让它看到 timeout_queue_ 现在非空了。重新计算下一个超时的精确时间点，并进入该时间点的定时等待。
        timeout_cv_.notify_one();
        
        Packet packet = Packet::createDataPacket(my_id_, batch);
        socket_->send(receiver_.ip, receiver_.port, packet.serialize());
    }
}

void Sender::retransmitLoop() 
{
    while (running_) 
    {
        std::unique_lock<std::mutex> lock(data_mutex_);
        
        //如果timeout_queue_为空，等待新的超时事件或停止信号
        if (timeout_queue_.empty()) 
        {
            timeout_cv_.wait(lock, [this] { return !timeout_queue_.empty() || !running_; });
            if (!running_) break;
            continue;
        }
        
        auto entry = timeout_queue_.top();
        //从timeout_queue_中取出第一个top元素，并等待直到其超时
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

void Sender::ackReceiveLoop() 
{
    while (running_) 
    {
        //线程5：阻塞接收ACK包，这里的socket_就是sender_socket_
        try 
        {
            auto [data, sender_ip, sender_port] = socket_->receive();
            Packet packet = Packet::deserialize(data);
            
            if (packet.type == MessageType::PERFECT_LINK_ACK) 
            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                for (uint32_t seq : packet.seq_numbers) 
                {
                    unacked_messages_.erase(seq);
                }
                //有ACK收到，可能会使得某些消息不再需要重传，唤醒retransmitLoop线程，
                //检查更新后的unacked_messages_是否还有timeout_queue_中需要重传的消息，从而重新计算下一个超时
                timeout_cv_.notify_one();
            }
        } 
        //当运行app：：shutdown时，关闭这个socket_以中断阻塞的receive调用，抛出异常
        catch (const std::exception& e) 
        {
            if (!running_) break;
        }
    }
    std::cout << "[DEBUG] ackReceiveLoop finished" << std::endl;
}

// ====================
// Receiver 
// ====================

Receiver::Receiver(UDPSocket* socket, Logger* logger)
    : socket_(socket), logger_(logger), flush_running_(false) {}

Receiver::~Receiver() {
    stop();
}

void Receiver::start() 
{
    //线程2：receiver定时flush发送ACK包
    flush_running_ = true;
    flush_thread_ = std::thread(&Receiver::flushLoop, this);
}

void Receiver::stop() 
{
    //设置线程2的停止标志，停止flushack
    flush_running_ = false;
    if (flush_thread_.joinable()) flush_thread_.join();
}

void Receiver::handle(const Packet& packet, const std::string& sender_ip, uint16_t sender_port) 
{
    if (packet.type != MessageType::PERFECT_LINK_DATA) return;
    
    std::string key = sender_ip + ":" + std::to_string(static_cast<unsigned int>(sender_port));
    std::lock_guard<std::mutex> lock(mtx_);
    
    uint32_t sender_id = packet.sender_id;
    std::set<uint32_t>& delivered = delivered_messages_[sender_id];
    
    if (delivered.size() >= MAX_DELIVERED_WINDOW) 
    {
        delivered.erase(delivered.begin());
    }
    
    for (uint32_t seq : packet.seq_numbers) 
    {
        if (delivered.find(seq) == delivered.end()) 
        {
            logger_->logDelivery(sender_id, seq);
            delivered.insert(seq);
        }
        pending_acks_[key].push_back(seq);
    }
    
    if (pending_acks_[key].size() >= ACK_BATCH_SIZE) 
    {
        std::vector<uint32_t> batch(pending_acks_[key].begin(), 
                                     pending_acks_[key].begin() + ACK_BATCH_SIZE);
        Packet ack = Packet::createAckPacket(batch);
        socket_->send(sender_ip, sender_port, ack.serialize());
        pending_acks_[key].erase(pending_acks_[key].begin(), 
                                  pending_acks_[key].begin() + ACK_BATCH_SIZE);
    }
}

void Receiver::flushLoop() 
{
    while (flush_running_) 
    {
        std::this_thread::sleep_for(ACK_FLUSH_TIMEOUT);
        
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [key, ack_list] : pending_acks_) 
        {
            if (ack_list.empty()) continue;
            
            size_t colon_pos = key.find(':');
            std::string sender_ip = key.substr(0, colon_pos);
            uint16_t sender_port = static_cast<uint16_t>(std::stoul(key.substr(colon_pos + 1)));
            
            while (!ack_list.empty()) 
            {
                size_t batch_size = std::min(ack_list.size(), static_cast<size_t>(ACK_BATCH_SIZE));
                std::vector<uint32_t> batch(ack_list.begin(), ack_list.begin() + batch_size);
                Packet ack = Packet::createAckPacket(batch);
                socket_->send(sender_ip, sender_port, ack.serialize());
                ack_list.erase(ack_list.begin(), ack_list.begin() + batch_size);
            }
        }
    }
}

void Receiver::flushAllPendingAcks() 
{
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

// =============================
// PerfectLinkApp Implementation
// =====================

PerfectLinkApp::PerfectLinkApp(uint32_t my_id, const std::vector<Host>& hosts,
                               uint32_t m, uint32_t receiver_id, const std::string& output_path)
    : my_id_(my_id), hosts_(hosts), m_(m), receiver_id_(receiver_id), running_(false) 
{

    Host my_host = findHost(my_id_);
    //receiver_socket_是线程1，接收DATA包，sender_socket_是线程5，接收ACK包
    receiver_socket_ = new UDPSocket(my_host.port);
    sender_socket_ = new UDPSocket(static_cast<uint16_t>(my_host.port + 1000));

    logger_ = new Logger(output_path);
    
    if (my_id_ != receiver_id_) 
    {
        Host receiver_host = findHost(receiver_id_);
        sender_ = new Sender(sender_socket_, my_id_, receiver_host, logger_);
    } 
    else 
    {
        sender_ = nullptr;
    }
    receiver_ = new Receiver(receiver_socket_, logger_);
}

PerfectLinkApp::~PerfectLinkApp() 
{
    shutdown();
    delete receiver_;
    delete sender_;
    delete logger_;
    delete sender_socket_;
    delete receiver_socket_;
}

void PerfectLinkApp::run() 
{
    running_ = true;
    //默认启动接收线程和receiver

    //线程1：receiver接受者，阻塞接收数据包
    receive_thread_ = std::thread(&PerfectLinkApp::receiveLoop, this);
    //线程2：receiver定时发送ACK包
    receiver_->start();
    
    //如果是sender，启动sender3个线程，发送m条消息，等待所有消息被ack
    if (sender_ != nullptr) 
    {
        sender_->start();
        for (uint32_t seq = 1; seq <= m_; seq++) 
        {
            sender_->send(seq);
        }
        logger_->flush();
        //单独线程阻塞等待所有消息被ack
        sender_->waitUntilAllAcked();
    } 
    else
    {
        std::cout << "[DEBUG] Receiver mode, listening for messages (will run until signal)" << std::endl;
    }
}

void PerfectLinkApp::shutdown()
{
    running_ = false;
    //线程1：receiverloop在使用这个receiver_socket_，关闭它以中断阻塞的receive调用
    receiver_socket_->close();
    //线程5：sender的ackreceiveloop在使用这个sender_socket_，关闭它以中断阻塞的receive调用
    sender_socket_->close();
    //线程2：receiver停止flushack线程
    receiver_->stop();
    if (sender_ != nullptr) sender_->stop();
    if (receive_thread_.joinable()) receive_thread_.detach();
    
    logger_->flush();
}


void PerfectLinkApp::receiveLoop() 
{
    //线程1：receiver接受者，阻塞接收数据包
    while (running_)
    {
        try 
        {
            auto [data, sender_ip, sender_port] = receiver_socket_->receive();
            Packet packet = Packet::deserialize(data);
            if (packet.type == MessageType::PERFECT_LINK_DATA) 
            {
                receiver_->handle(packet, sender_ip, sender_port);
            }
        } 
        catch (const std::exception&)
        //当app：：shutdown时，receiver_socket_被关闭，会抛出异常，跳出阻塞的receive调用
        {
            if (!running_) 
            {
                std::cout << "[DEBUG] receiveLoop exiting due to !running_" << std::endl;
                break;
            }
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