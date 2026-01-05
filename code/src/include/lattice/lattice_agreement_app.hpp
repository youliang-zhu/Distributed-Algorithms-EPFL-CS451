#ifndef LATTICE_AGREEMENT_APP_HPP
#define LATTICE_AGREEMENT_APP_HPP

#include "common/types.hpp"
#include "common/logger.hpp"
#include "network/udp_socket.hpp"
#include "network/message.hpp"
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <queue>

namespace milestone3 {

// Forward declaration
class LatticeAgreementApp;

// Proposer state for a single slot
struct ProposerState {
    bool active = false;
    uint32_t ack_count = 0;
    uint32_t nack_count = 0;
    uint32_t active_proposal_number = 0;
    std::set<uint32_t> proposed_value;
    std::set<uint32_t> responded_processes_this_iteration;
};

// Acceptor state for a single slot
struct AcceptorState {
    std::set<uint32_t> accepted_value;
};

// Proposal Retransmitter (unified scheduler)
class ProposalRetransmitter {
public:
    explicit ProposalRetransmitter(LatticeAgreementApp* app);
    ~ProposalRetransmitter();

    void start();
    void stop();
    void startRetransmitting(uint32_t slot);

private:
    void retransmitLoop();

    LatticeAgreementApp* app_;
    std::set<uint32_t> active_slots_;
    std::mutex mutex_;
    std::thread retransmit_thread_;
    std::atomic<bool> running_{false};
};

// Output Manager (streaming output with ordering)
class OutputManager {
public:
    explicit OutputManager(Logger* logger);

    void recordDecision(uint32_t slot, const std::set<uint32_t>& decision);
    void finalFlush();

private:
    Logger* logger_;
    uint32_t next_expected_slot_ = 0;
    std::map<uint32_t, std::set<uint32_t>> pending_decisions_;
    std::mutex mutex_;
};

// Slot Pipeline Manager (limits concurrent slots)
class SlotPipelineManager {
public:
    SlotPipelineManager(uint32_t max_concurrent_slots, LatticeAgreementApp* app);

    void init(uint32_t total_slots);
    void onSlotDecided(uint32_t slot);

private:
    void tryStartNextSlot();
    void startSlot(uint32_t slot);

    uint32_t max_concurrent_slots_;
    LatticeAgreementApp* app_;
    std::queue<uint32_t> pending_slots_;
    std::set<uint32_t> active_slots_;
    std::mutex mutex_;

    friend class LatticeAgreementApp;
};

class LatticeAgreementApp {
public:
    LatticeAgreementApp(uint32_t my_id, const std::vector<Host>& hosts,
                        const LatticeAgreementConfig& config,
                        const std::string& output_path);
    ~LatticeAgreementApp();

    void run();
    void shutdown();

    // For Retransmitter
    bool isSlotActive(uint32_t slot) const;
    void broadcastProposal(uint32_t slot);

private:
    // Core configuration
    uint32_t my_id_;
    std::vector<Host> hosts_;
    uint32_t n_processes_;
    uint32_t f_;           // Max failures
    uint32_t majority_;    // f + 1

    LatticeAgreementConfig config_;

    // Network components
    UDPSocket* socket_;
    Logger* logger_;

    // Proposer and Acceptor states
    std::map<uint32_t, ProposerState> proposer_state_;
    std::map<uint32_t, AcceptorState> acceptor_state_;
    std::set<uint32_t> decided_slots_;
    std::map<uint32_t, std::set<uint32_t>> decided_values_;  // slot -> decided value

    mutable std::mutex state_mutex_;

    // Auxiliary components
    ProposalRetransmitter retransmitter_;
    OutputManager* output_manager_;
    SlotPipelineManager slot_pipeline_;

    // Threads
    std::thread receive_thread_;
    std::atomic<bool> running_;

    // Core logic
    void propose(uint32_t slot, const std::set<uint32_t>& proposal);
    void handleProposal(uint32_t slot, uint32_t pn, uint32_t sender_id,
                        const std::set<uint32_t>& proposed_value);
    void handleAck(uint32_t slot, uint32_t pn, uint32_t sender_id);
    void handleNack(uint32_t slot, uint32_t pn, uint32_t sender_id,
                    const std::set<uint32_t>& value);

    void startNewIteration(uint32_t slot);
    void decide(uint32_t slot, const std::set<uint32_t>& value);

    // Communication
    void sendAck(uint32_t dest_id, uint32_t slot, uint32_t pn);
    void sendNack(uint32_t dest_id, uint32_t slot, uint32_t pn,
                  const std::set<uint32_t>& value);

    void receiveLoop();

    // Helpers
    Host findHost(uint32_t id) const;
    uint32_t getProcessIdFromAddress(const std::string& ip, uint16_t port) const;

    friend class ProposalRetransmitter;
    friend class SlotPipelineManager;
};

}

#endif
