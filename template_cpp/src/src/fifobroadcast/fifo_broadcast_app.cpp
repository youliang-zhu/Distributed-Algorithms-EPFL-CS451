#include "fifobroadcast/fifo_broadcast_app.hpp"
#include <iostream>

namespace milestone2 {

FIFOBroadcastApp::FIFOBroadcastApp(uint32_t my_id, const std::vector<Host>& hosts,
                                   uint32_t m, const std::string& output_path)
    : my_id_(my_id), hosts_(hosts), m_(m), running_(false) {
    
    n_processes_ = hosts_.size();
    majority_ = n_processes_ / 2 + 1;
    
    Host my_host = findHost(my_id_);
    receiver_socket_ = new UDPSocket(my_host.port);
    sender_socket_ = new UDPSocket(static_cast<uint16_t>(my_host.port + 1000));
    
    logger_ = new Logger(output_path);
    
    for (const Host& host : hosts_) {
        if (host.id != my_id_) {
            senders_[host.id] = new milestone1::Sender(sender_socket_, my_id_, host, logger_);
        }
    }
    
    receiver_ = new milestone1::Receiver(receiver_socket_, logger_);
    
    for (const Host& host : hosts_) {
        next_[host.id] = 1;
    }
}

FIFOBroadcastApp::~FIFOBroadcastApp() {
    shutdown();
    for (auto& [id, sender] : senders_) {
        delete sender;
    }
    delete receiver_;
    delete logger_;
    delete sender_socket_;
    delete receiver_socket_;
}

void FIFOBroadcastApp::run() {
    running_ = true;
    
    receive_thread_ = std::thread(&FIFOBroadcastApp::receiveLoop, this);
    receiver_->start();
    
    for (auto& [id, sender] : senders_) {
        sender->start();
    }
    
    for (uint32_t seq = 1; seq <= m_; seq++) {
        urbBroadcast(my_id_, seq);
    }
    
    logger_->flush();
}

void FIFOBroadcastApp::shutdown() {
    running_ = false;
    
    receiver_socket_->close();
    sender_socket_->close();
    
    receiver_->stop();
    for (auto& [id, sender] : senders_) {
        sender->stop();
    }
    
    if (receive_thread_.joinable()) receive_thread_.detach();
    
    logger_->flush();
}

void FIFOBroadcastApp::urbBroadcast(uint32_t sender_id, uint32_t seq) {
    MessageId msg_id = {sender_id, seq};
    
    {
        std::lock_guard<std::mutex> lock(receiver_state_mutex_);
        
        if (sender_id == my_id_) {
            logger_->logBroadcast(seq);
        }
        
        forwarded_.insert(msg_id);
        urb_ack_list_[msg_id].insert(my_id_);
        
        if (sender_id == my_id_) {
            urb_ack_list_[msg_id].insert(sender_id);
        }
    }
    
    for (auto& [target_id, sender] : senders_) {
        sender->send(sender_id, seq);
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

void FIFOBroadcastApp::receiveLoop() {
    while (running_) {
        try {
            auto [data, sender_ip, sender_port] = receiver_socket_->receive();
            Packet packet = Packet::deserialize(data);
            if (packet.type == MessageType::PERFECT_LINK_DATA) {
                handlePacket(packet, sender_ip, sender_port);
            }
        } catch (const std::exception&) {
            if (!running_) break;
        }
    }
}

void FIFOBroadcastApp::handlePacket(const Packet& packet, const std::string& sender_ip, uint16_t sender_port) {
    uint32_t udp_source_id = getProcessIdFromAddress(sender_ip, sender_port);
    uint32_t original_sender = packet.sender_id;
    
    receiver_->handle(packet, sender_ip, sender_port);
    
    for (uint32_t seq : packet.seq_numbers) {
        MessageId msg_id = {original_sender, seq};
        
        bool should_forward = false;
        bool should_deliver = false;
        
        {
            std::lock_guard<std::mutex> lock(receiver_state_mutex_);
            
            urb_ack_list_[msg_id].insert(udp_source_id);
            urb_ack_list_[msg_id].insert(original_sender);
            
            if (forwarded_.find(msg_id) == forwarded_.end()) {
                forwarded_.insert(msg_id);
                should_forward = true;
            }
            
            if (!urb_delivered_.count(msg_id) && urb_ack_list_[msg_id].size() >= majority_) {
                urb_delivered_.insert(msg_id);
                urb_ack_list_.erase(msg_id);
                should_deliver = true;
            }
        }
        
        if (should_forward) {
            for (auto& [target_id, sender] : senders_) {
                sender->send(original_sender, seq);
            }
        }
        
        if (should_deliver) {
            std::lock_guard<std::mutex> lock(receiver_state_mutex_);
            fifoDeliver(original_sender, seq);
        }
    }
}

void FIFOBroadcastApp::fifoDeliver(uint32_t sender_id, uint32_t seq) {
    if (seq == next_[sender_id]) {
        logger_->logDelivery(sender_id, seq);
        next_[sender_id]++;
        
        while (pending_[sender_id].count(next_[sender_id])) {
            uint32_t next_seq = next_[sender_id];
            logger_->logDelivery(sender_id, next_seq);
            pending_[sender_id].erase(next_seq);
            next_[sender_id]++;
        }
    } else {
        pending_[sender_id][seq] = {sender_id, seq};
    }
}

Host FIFOBroadcastApp::findHost(uint32_t id) const {
    for (const Host& host : hosts_) {
        if (host.id == id) return host;
    }
    return Host();
}

uint32_t FIFOBroadcastApp::getProcessIdFromAddress(const std::string& ip, uint16_t port) const {
    uint16_t base_port = port >= 12000 ? port - 1000 : port;
    for (const Host& host : hosts_) {
        if (host.port == base_port) return host.id;
    }
    return 0;
}

}
