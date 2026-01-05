#include "lattice/lattice_agreement_app.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>

namespace milestone3 {

// ============= ProposalRetransmitter =============
//负责定时重传active的proposal

ProposalRetransmitter::ProposalRetransmitter(LatticeAgreementApp* app)
    : app_(app) {}

ProposalRetransmitter::~ProposalRetransmitter() {
    stop();
}

void ProposalRetransmitter::start() 
{
    running_ = true;
    retransmit_thread_ = std::thread(&ProposalRetransmitter::retransmitLoop, this);
}

void ProposalRetransmitter::stop()
{
    running_ = false;
    if (retransmit_thread_.joinable()) {
        retransmit_thread_.join();
    }
}

void ProposalRetransmitter::startRetransmitting(uint32_t slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_slots_.insert(slot);
}

void ProposalRetransmitter::retransmitLoop() {
    while (running_) {
        std::vector<uint32_t> to_remove;
        //int retransmit_count = 0;  //调试用，统计每轮重传了多少slot

        {
            std::lock_guard<std::mutex> lock(mutex_);

            for(uint32_t slot : active_slots_) {
                if (app_->isSlotActive(slot)) {
                    app_->broadcastProposal(slot);
                    // retransmit_count++;
                } else {
                    to_remove.push_back(slot);
                }
            }

            //延迟删除，避免迭代器失效
            for (uint32_t slot : to_remove) {
                active_slots_.erase(slot);
            }
        }

        // 50ms重传间隔，可以调整
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

//=============OutputManager =============
//管理输出顺序，保证slot按序输出

OutputManager::OutputManager(Logger* logger)
    : logger_(logger) {}

void OutputManager::recordDecision(uint32_t slot, const std::set<uint32_t>& decision) {
    std::lock_guard<std::mutex> lock(mutex_);

    pending_decisions_[slot] = decision;

    //只输出连续的已决定slot
    while (pending_decisions_.count(next_expected_slot_))
    {
        logger_->logDecision(pending_decisions_[next_expected_slot_]);
        pending_decisions_.erase(next_expected_slot_);
        next_expected_slot_++;
    }
}

void OutputManager::finalFlush() 
{
    std::lock_guard<std::mutex> lock(mutex_);

    //finalFlush只在shutdown时调用,从slot 0开始，遇到第一个没决定的就停止
    for (uint32_t slot = 0; ; slot++) {
        if(!pending_decisions_.count(slot)) {
            break;
        }
        logger_->logDecision(pending_decisions_[slot]);
    }
}

// ============= SlotPipelineManager=============
// 控制并发slot数量，避免同时处理太多slot导致内存爆炸.max_concurrent_slots 设成10，经过测试效果还行
SlotPipelineManager::SlotPipelineManager(uint32_t max_concurrent_slots, LatticeAgreementApp* app)
    : max_concurrent_slots_(max_concurrent_slots), app_(app) {}

void SlotPipelineManager::init(uint32_t total_slots) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (uint32_t slot = 0; slot < total_slots; slot++) {
        pending_slots_.push(slot);
    }

    // Start initial batch
    tryStartNextSlot();
}

void SlotPipelineManager::onSlotDecided(uint32_t slot) 
{
    std::lock_guard<std::mutex> lock(mutex_);

    active_slots_.erase(slot);
    tryStartNextSlot();
}

void SlotPipelineManager::tryStartNextSlot() 
{
    //调用者必须持有mutex_
    while(active_slots_.size() < max_concurrent_slots_ && !pending_slots_.empty()) {
        uint32_t slot = pending_slots_.front();
        pending_slots_.pop();
        startSlot(slot);
        active_slots_.insert(slot);
    }
}

void SlotPipelineManager::startSlot(uint32_t slot) 
{
    //从config读取该slot的proposal
    if(slot >= app_->config_.proposal_sets.size()) {
        return; //不应该发生
    }

    std::set<uint32_t> proposal(app_->config_.proposal_sets[slot].begin(),
                                 app_->config_.proposal_sets[slot].end());

    app_->propose(slot, proposal);
}

// ============= LatticeAgreementApp主类 =============

LatticeAgreementApp::LatticeAgreementApp(uint32_t my_id, const std::vector<Host>& hosts,
                                         const LatticeAgreementConfig& config,
                                         const std::string& output_path)
    : my_id_(my_id), hosts_(hosts), config_(config),
      retransmitter_(this), output_manager_(nullptr),
      slot_pipeline_(10, this), running_(false) //初始化列表顺序要和声明一致
{
    n_processes_ = static_cast<uint32_t>(hosts_.size());
    f_ = (n_processes_ - 1) / 2;  // 最多容忍f个进程挂掉
    majority_ = f_ + 1;  // 多数派 = f+1

    Host my_host = findHost(my_id_);
    logger_ = new Logger(output_path);

    //OutputManager复用同一个Logger
    output_manager_ = new OutputManager(logger_);

    socket_ = new UDPSocket(my_host.port);

    // std::cerr << "Process " << my_id_ << " initialized, n=" << n_processes_
    //           << ", f=" << f_ << ", majority=" << majority_ << std::endl;
}

LatticeAgreementApp::~LatticeAgreementApp() 
{
    shutdown();
    delete output_manager_;
    delete logger_;
    delete socket_;
}

void LatticeAgreementApp::run() 
{
    running_ = true;

    //启动接收线程和重传线程
    receive_thread_ = std::thread(&LatticeAgreementApp::receiveLoop, this);
    retransmitter_.start();

    // 等待其他进程启动，900ms是经验值
    // TODO(youliang):这个等待时间可能需要根据实际情况调整
    std::this_thread::sleep_for(std::chrono::milliseconds(900));

    //开始处理slot
    slot_pipeline_.init(config_.proposals);

    //主线程就在这里等着slot由pipeline管理
}

void LatticeAgreementApp::shutdown() {
    running_ = false;

    if (socket_) socket_->close();
    retransmitter_.stop();

    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    output_manager_->finalFlush();
    logger_->flush();
}

void LatticeAgreementApp::propose(uint32_t slot, const std::set<uint32_t>& proposal) 
{
    // std::cerr << "[DEBUG] Process " << my_id_ << " proposing slot " << slot << std::endl;

    std::lock_guard<std::mutex> lock(state_mutex_);

    auto& state = proposer_state_[slot];
    state.proposed_value = proposal;
    state.active = true;
    state.active_proposal_number = 1;  //pn从1开始
    state.ack_count = 0;
    state.nack_count = 0;
    state.responded_processes_this_iteration.clear();

    //开始定时重传
    retransmitter_.startRetransmitting(slot);
}

void LatticeAgreementApp::handleAck(uint32_t slot, uint32_t pn, uint32_t sender_id) 
{
    bool should_decide = false;
    std::set<uint32_t> decide_value;
    
    {
        std::lock_guard<std::mutex> lock(state_mutex_);

        auto it = proposer_state_.find(slot);
        if(it == proposer_state_.end()) {
            // std::cerr << "[received ACK for slot " << slot << " but no state" << std::endl;
            return;
        }
        auto& state = it->second;

        //检查pn是否匹配
        if(pn != state.active_proposal_number) return;

        //检查是否已经决定
        if (!state.active) return;
        if(state.responded_processes_this_iteration.count(sender_id)) return;

        //记录响应
        state.responded_processes_this_iteration.insert(sender_id);
        state.ack_count++;

        // std::cerr << "[DEBUG] slot " << slot << " ack_count=" << state.ack_count << std::endl;

        //检查是否达到多数派
        if (state.ack_count >= majority_) 
        {
            should_decide = true;
            decide_value = state.proposed_value;
            
            //在锁内完成状态更新
            state.active = false;
            output_manager_->recordDecision(slot, decide_value);
            decided_slots_.insert(slot);
            decided_values_[slot] = decide_value;
            proposer_state_.erase(slot);
            acceptor_state_.erase(slot);
        }
        //检查是否需要开始新一轮
        else if (state.nack_count > 0 &&
            state.ack_count + state.nack_count >= majority_) 
        {
            startNewIteration(slot);
        }
    }
    //FIXME:之前这里有死锁bug，因为在持有state_mutex_时调用onSlotDecided
    //       而onSlotDecided会调用propose，propose又需要state_mutex_
    //       现在改成先释放锁再调用

    if(should_decide) {
        // printf("[DEBUG] decided slot %u\n", slot);  // for debugging
        slot_pipeline_.onSlotDecided(slot);
    }
}

void LatticeAgreementApp::handleNack(uint32_t slot, uint32_t pn, uint32_t sender_id,
                                     const std::set<uint32_t>& value) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    auto& state = proposer_state_[slot];

    if(pn != state.active_proposal_number) return;  //pn不匹配
    if (!state.active) return;  //已经决定了
    if(state.responded_processes_this_iteration.count(sender_id)) return;  //重复响应

    state.responded_processes_this_iteration.insert(sender_id);
    state.nack_count++;

    //合并value，取并集
    std::set<uint32_t> merged;
    std::set_union(state.proposed_value.begin(), state.proposed_value.end(),
                   value.begin(), value.end(),
                   std::inserter(merged, merged.begin()));
    state.proposed_value = merged;

    //收到nack且响应数达到多数派，开始新一轮
    if (state.nack_count > 0 &&
        state.ack_count + state.nack_count >= majority_)
    {
        startNewIteration(slot);
    }
}

void LatticeAgreementApp::handleProposal(uint32_t slot, uint32_t pn, uint32_t sender_id,
                                         const std::set<uint32_t>& proposed_value) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // BUG FIX: 已经决定的slot也要响应，否则其他进程可能卡住
    // 这个bug卡了我两天...
    if(decided_slots_.count(slot)) {
        const auto& decided_value = decided_values_[slot];
        bool is_subset = std::includes(proposed_value.begin(), proposed_value.end(),
                                        decided_value.begin(), decided_value.end());
        // std::cerr << "[DEBUG] already decided slot " << slot << std::endl;
        if (is_subset) {
            sendAck(sender_id, slot, pn);
        } else {
            //合并后发NACK
            std::set<uint32_t> merged;
            std::set_union(decided_value.begin(), decided_value.end(),
                           proposed_value.begin(), proposed_value.end(),
                           std::inserter(merged, merged.begin()));
            sendNack(sender_id, slot, pn, merged);
        }
        return;
    }

    auto& state = acceptor_state_[slot];

    //检查子集关系：accepted_value ⊆ proposed_value ?
    bool is_subset = std::includes(proposed_value.begin(), proposed_value.end(),
                                    state.accepted_value.begin(),
                                    state.accepted_value.end());

    if(is_subset) {
        state.accepted_value = proposed_value;
        sendAck(sender_id, slot, pn);
    } else {
        std::set<uint32_t> merged;
        std::set_union(state.accepted_value.begin(), state.accepted_value.end(),
                       proposed_value.begin(), proposed_value.end(),
                       std::inserter(merged, merged.begin()));
        state.accepted_value = merged;
        sendNack(sender_id, slot, pn, state.accepted_value);
    }
}

void LatticeAgreementApp::startNewIteration(uint32_t slot) 
{
    //调用者已经持有state_mutex_
    auto& state = proposer_state_[slot];
    state.active_proposal_number++;  //pn+1
    state.ack_count = 0;
    state.nack_count = 0;
    state.responded_processes_this_iteration.clear();

    //retransmitter会自动用新的pn广播
}

// deprecated: 这个函数已经不用了，决定逻辑移到handleAck里面了
// 保留是因为可能以后还会用到
void LatticeAgreementApp::decide(uint32_t slot, const std::set<uint32_t>& value) {
    //调用者已持有state_mutex_

    /*
    std::cerr << "[DEBUG] deciding slot " << slot << ": {";
    bool first = true;
    for (uint32_t v : value) {
        if (!first) std::cerr << ", ";
        std::cerr << v;
        first = false;
    }
    std::cerr << "}" << std::endl;
    */

    auto& state = proposer_state_[slot];
    state.active = false;

    output_manager_->recordDecision(slot, value);
    decided_slots_.insert(slot);
    decided_values_[slot] = value;

    proposer_state_.erase(slot);
    acceptor_state_.erase(slot);

    slot_pipeline_.onSlotDecided(slot);  //这里会死锁！见handleAck的注释
}

bool LatticeAgreementApp::isSlotActive(uint32_t slot) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = proposer_state_.find(slot);
    return it != proposer_state_.end() && it->second.active;
}

void LatticeAgreementApp::broadcastProposal(uint32_t slot) {
    uint32_t pn;
    std::set<uint32_t> proposal_value;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);

        auto it = proposer_state_.find(slot);
        if(it == proposer_state_.end() || !it->second.active) return;

        auto& state = it->second;
        pn = state.active_proposal_number;
        proposal_value = state.proposed_value;

        Packet pkt = Packet::createProposalPacket(slot, pn, my_id_, proposal_value);

        //BEB广播，不包括自己
        for(const auto& host : hosts_) {
            if(host.id != my_id_) {
                try {
                    socket_->send(host.ip, host.port, pkt.serialize());
                } catch (...) {
                    //发送失败忽略，反正会重传
                }
            }
        }
    }

    //自己也作为acceptor处理这个proposal
    handleProposal(slot, pn, my_id_, proposal_value);
}

void LatticeAgreementApp::sendAck(uint32_t dest_id, uint32_t slot, uint32_t pn) {
    Packet ack = Packet::createLAAckPacket(slot, pn);
    Host dest = findHost(dest_id);
    try {
        socket_->send(dest.ip, dest.port, ack.serialize());
    } catch(...) {}
}

void LatticeAgreementApp::sendNack(uint32_t dest_id, uint32_t slot, uint32_t pn,
                                   const std::set<uint32_t>& value) {
    Packet nack = Packet::createNackPacket(slot, pn, value);
    Host target = findHost(dest_id);  //这里用target，上面用dest，懒得改了
    try {
        socket_->send(target.ip, target.port, nack.serialize());
    } catch(...) {}
}

void LatticeAgreementApp::receiveLoop() {
    while (running_)
    {
        try {
            auto [data, sender_ip, sender_port] = socket_->receive();
            Packet packet = Packet::deserialize(data);

            uint32_t sender_id = getProcessIdFromAddress(sender_ip, sender_port);

            //根据消息类型分发
            if (packet.type == MessageType::PROPOSAL) {
                handleProposal(packet.slot_number, packet.proposal_number,
                               packet.sender_id, packet.value_set);
            }
            else if(packet.type == MessageType::ACK) {
                handleAck(packet.slot_number, packet.proposal_number, sender_id);
            }
            else if(packet.type == MessageType::NACK) {
                handleNack(packet.slot_number, packet.proposal_number,
                           sender_id, packet.value_set);
            }
            // else: 未知消息类型，忽略
        } catch(...) {
            if (!running_) break;
        }
    }
}

Host LatticeAgreementApp::findHost(uint32_t id) const {
    for(const Host& host : hosts_) {
        if(host.id == id) return host;
    }
    return Host();  //没找到返回空Host
}

uint32_t LatticeAgreementApp::getProcessIdFromAddress(const std::string& ip, uint16_t port) const {
    // ip参数其实没用到，因为我们都在localhost上跑
    // 只用port就能区分进程
    (void)ip;  //消除unused参数警告
    for(const Host& host : hosts_) {
        if(host.port == port) return host.id;
    }
    return 0;
}

}