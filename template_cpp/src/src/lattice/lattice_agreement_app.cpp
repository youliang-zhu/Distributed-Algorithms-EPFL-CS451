#include "lattice/lattice_agreement_app.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>

namespace milestone3 {

// ============= ProposalRetransmitter Implementation =============

ProposalRetransmitter::ProposalRetransmitter(LatticeAgreementApp* app)
    : app_(app) {}

ProposalRetransmitter::~ProposalRetransmitter() {
    stop();
}

void ProposalRetransmitter::start() {
    running_ = true;
    retransmit_thread_ = std::thread(&ProposalRetransmitter::retransmitLoop, this);
}

void ProposalRetransmitter::stop() {
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

        {
            std::lock_guard<std::mutex> lock(mutex_);

            for (uint32_t slot : active_slots_) {
                if (app_->isSlotActive(slot)) {
                    app_->broadcastProposal(slot);
                } else {
                    to_remove.push_back(slot);
                }
            }

            // Delayed removal to avoid iterator invalidation
            for (uint32_t slot : to_remove) {
                active_slots_.erase(slot);
            }
        }

        // Wait before next retransmission
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ============= OutputManager Implementation =============

OutputManager::OutputManager(Logger* logger)
    : logger_(logger) {}

void OutputManager::recordDecision(uint32_t slot, const std::set<uint32_t>& decision) {
    std::lock_guard<std::mutex> lock(mutex_);

    pending_decisions_[slot] = decision;

    // Stream output: output consecutive decided slots
    while (pending_decisions_.count(next_expected_slot_)) {
        logger_->logDecision(pending_decisions_[next_expected_slot_]);
        pending_decisions_.erase(next_expected_slot_);
        next_expected_slot_++;
    }
}

void OutputManager::finalFlush() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Only output consecutive decided slots from 0
    for (uint32_t slot = 0; ; slot++) {
        if (!pending_decisions_.count(slot)) {
            break; // First undecided slot, stop
        }
        logger_->logDecision(pending_decisions_[slot]);
    }
}

// ============= SlotPipelineManager Implementation =============

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

void SlotPipelineManager::onSlotDecided(uint32_t slot) {
    std::lock_guard<std::mutex> lock(mutex_);

    active_slots_.erase(slot);
    tryStartNextSlot();
}

void SlotPipelineManager::tryStartNextSlot() {
    // Note: caller must hold mutex_

    while (active_slots_.size() < max_concurrent_slots_ && !pending_slots_.empty()) {
        uint32_t slot = pending_slots_.front();
        pending_slots_.pop();
        startSlot(slot);
        active_slots_.insert(slot);
    }
}

void SlotPipelineManager::startSlot(uint32_t slot) {
    // Read proposal from config
    if (slot >= app_->config_.proposal_sets.size()) {
        return; // Should not happen
    }

    std::set<uint32_t> proposal(app_->config_.proposal_sets[slot].begin(),
                                 app_->config_.proposal_sets[slot].end());

    app_->propose(slot, proposal);
}

// ============= LatticeAgreementApp Implementation =============

LatticeAgreementApp::LatticeAgreementApp(uint32_t my_id, const std::vector<Host>& hosts,
                                         const LatticeAgreementConfig& config,
                                         const std::string& output_path)
    : my_id_(my_id), hosts_(hosts), config_(config),
      retransmitter_(this), output_manager_(nullptr),
      slot_pipeline_(10, this), running_(false) // Must match declaration order
{
    n_processes_ = static_cast<uint32_t>(hosts_.size());
    f_ = (n_processes_ - 1) / 2;
    majority_ = f_ + 1;

    Host my_host = findHost(my_id_);
    logger_ = new Logger(output_path);

    // OutputManager reuses the same Logger instance
    output_manager_ = new OutputManager(logger_);

    socket_ = new UDPSocket(my_host.port);

    std::cerr << "[DEBUG] Process " << my_id_ << " initialized, n=" << n_processes_
              << ", f=" << f_ << ", majority=" << majority_ << std::endl;
}

LatticeAgreementApp::~LatticeAgreementApp() {
    shutdown();
    delete output_manager_;
    delete logger_;
    delete socket_;
}

void LatticeAgreementApp::run() {
    running_ = true;

    // Start threads
    receive_thread_ = std::thread(&LatticeAgreementApp::receiveLoop, this);
    retransmitter_.start();

    // Cold start wait
    std::this_thread::sleep_for(std::chrono::milliseconds(900));

    // Initialize slot pipeline
    slot_pipeline_.init(config_.proposals);

    // Main thread waits (slots are managed by pipeline)
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

void LatticeAgreementApp::propose(uint32_t slot, const std::set<uint32_t>& proposal) {
    std::cerr << "[DEBUG] Process " << my_id_ << " proposing slot " << slot << std::endl;

    std::lock_guard<std::mutex> lock(state_mutex_);

    auto& state = proposer_state_[slot];
    state.proposed_value = proposal;
    state.active = true;
    state.active_proposal_number = 1;
    state.ack_count = 0;
    state.nack_count = 0;
    state.responded_processes_this_iteration.clear();

    // Start retransmission
    retransmitter_.startRetransmitting(slot);
}

void LatticeAgreementApp::handleAck(uint32_t slot, uint32_t pn, uint32_t sender_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    auto& state = proposer_state_[slot];

    // Check: proposal_number match
    if (pn != state.active_proposal_number) return;

    // Check: already decided
    if (!state.active) return;

    // Check: duplicate response
    if (state.responded_processes_this_iteration.count(sender_id)) return;

    // Record response
    state.responded_processes_this_iteration.insert(sender_id);
    state.ack_count++;

    // Check decision condition
    if (state.ack_count >= majority_) {
        decide(slot, state.proposed_value);
        return;
    }

    // Check new iteration condition
    if (state.nack_count > 0 &&
        state.ack_count + state.nack_count >= majority_) {
        startNewIteration(slot);
    }
}

void LatticeAgreementApp::handleNack(uint32_t slot, uint32_t pn, uint32_t sender_id,
                                     const std::set<uint32_t>& value) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    auto& state = proposer_state_[slot];

    // Check: proposal_number match
    if (pn != state.active_proposal_number) return;

    // Check: already decided
    if (!state.active) return;

    // Check: duplicate response
    if (state.responded_processes_this_iteration.count(sender_id)) return;

    // Record response
    state.responded_processes_this_iteration.insert(sender_id);
    state.nack_count++;

    // Merge value
    std::set<uint32_t> merged;
    std::set_union(state.proposed_value.begin(), state.proposed_value.end(),
                   value.begin(), value.end(),
                   std::inserter(merged, merged.begin()));
    state.proposed_value = merged;

    // Check new iteration condition
    if (state.nack_count > 0 &&
        state.ack_count + state.nack_count >= majority_) {
        startNewIteration(slot);
    }
}

void LatticeAgreementApp::handleProposal(uint32_t slot, uint32_t pn, uint32_t sender_id,
                                         const std::set<uint32_t>& proposed_value) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // Check: already decided
    if (decided_slots_.count(slot)) return;

    auto& state = acceptor_state_[slot];

    // Check subset relationship
    bool is_subset = std::includes(proposed_value.begin(), proposed_value.end(),
                                    state.accepted_value.begin(),
                                    state.accepted_value.end());

    if (is_subset) {
        // accepted_value ⊆ proposed_value
        state.accepted_value = proposed_value;
        sendAck(sender_id, slot, pn);
    } else {
        // accepted_value ⊄ proposed_value
        std::set<uint32_t> merged;
        std::set_union(state.accepted_value.begin(), state.accepted_value.end(),
                       proposed_value.begin(), proposed_value.end(),
                       std::inserter(merged, merged.begin()));
        state.accepted_value = merged;
        sendNack(sender_id, slot, pn, state.accepted_value);
    }
}

void LatticeAgreementApp::startNewIteration(uint32_t slot) {
    // Note: caller already holds state_mutex_

    auto& state = proposer_state_[slot];
    state.active_proposal_number++;
    state.ack_count = 0;
    state.nack_count = 0;
    state.responded_processes_this_iteration.clear();

    // Retransmitter will automatically broadcast with new pn
}

void LatticeAgreementApp::decide(uint32_t slot, const std::set<uint32_t>& value) {
    // Note: caller already holds state_mutex_

    std::cerr << "[DEBUG] Process " << my_id_ << " deciding slot " << slot << ": {";
    bool first = true;
    for (uint32_t v : value) {
        if (!first) std::cerr << ", ";
        std::cerr << v;
        first = false;
    }
    std::cerr << "}" << std::endl;

    auto& state = proposer_state_[slot];
    state.active = false; // Stop retransmission

    // Record decision
    output_manager_->recordDecision(slot, value);
    decided_slots_.insert(slot);

    // Memory cleanup
    proposer_state_.erase(slot);
    acceptor_state_.erase(slot);

    // Notify pipeline manager
    slot_pipeline_.onSlotDecided(slot);
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
        if (it == proposer_state_.end() || !it->second.active) return;

        auto& state = it->second;
        pn = state.active_proposal_number;
        proposal_value = state.proposed_value;

        Packet packet = Packet::createProposalPacket(slot, pn, my_id_, proposal_value);

        // BEB broadcast (not including self)
        for (const auto& host : hosts_) {
            if (host.id != my_id_) {
                try {
                    socket_->send(host.ip, host.port, packet.serialize());
                } catch (...) {
                    // Ignore send errors
                }
            }
        }
    } // Release lock before handling own proposal

    // Process own proposal (as acceptor)
    handleProposal(slot, pn, my_id_, proposal_value);
}

void LatticeAgreementApp::sendAck(uint32_t dest_id, uint32_t slot, uint32_t pn) {
    Packet ack = Packet::createLAAckPacket(slot, pn);
    Host dest = findHost(dest_id);
    try {
        socket_->send(dest.ip, dest.port, ack.serialize());
    } catch (...) {
        // Ignore send errors
    }
}

void LatticeAgreementApp::sendNack(uint32_t dest_id, uint32_t slot, uint32_t pn,
                                   const std::set<uint32_t>& value) {
    Packet nack = Packet::createNackPacket(slot, pn, value);
    Host dest = findHost(dest_id);
    try {
        socket_->send(dest.ip, dest.port, nack.serialize());
    } catch (...) {
        // Ignore send errors
    }
}

void LatticeAgreementApp::receiveLoop() {
    while (running_) {
        try {
            auto [data, sender_ip, sender_port] = socket_->receive();
            Packet packet = Packet::deserialize(data);

            uint32_t sender_id = getProcessIdFromAddress(sender_ip, sender_port);

            // Dispatch based on message type
            if (packet.type == MessageType::PROPOSAL) {
                handleProposal(packet.slot_number, packet.proposal_number,
                               packet.sender_id, packet.value_set);
            }
            else if (packet.type == MessageType::ACK) {
                handleAck(packet.slot_number, packet.proposal_number, sender_id);
            }
            else if (packet.type == MessageType::NACK) {
                handleNack(packet.slot_number, packet.proposal_number,
                           sender_id, packet.value_set);
            }
        } catch (...) {
            if (!running_) break;
        }
    }
}

Host LatticeAgreementApp::findHost(uint32_t id) const {
    for (const Host& host : hosts_) {
        if (host.id == id) return host;
    }
    return Host();
}

uint32_t LatticeAgreementApp::getProcessIdFromAddress(const std::string& ip, uint16_t port) const {
    for (const Host& host : hosts_) {
        if (host.port == port) return host.id;
    }
    return 0;
}

}
