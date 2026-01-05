#ifndef CONFIG_HPP
#define CONFIG_HPP

#include "types.hpp"
#include <string>

enum class ConfigType 
{
    PERFECT_LINK,
    FIFO_BROADCAST,
    LATTICE_AGREEMENT
};

class Config 
{
public:
    static Config parse(const std::string& config_path);
    ConfigType getType() const { return type_; }
    
    const PerfectLinkConfig& getPerfectLinkConfig() const { return perfect_link_config_; }
    const FIFOBroadcastConfig& getFIFOBroadcastConfig() const { return fifo_broadcast_config_; }
    const LatticeAgreementConfig& getLatticeAgreementConfig() const { return lattice_agreement_config_; }

private:
    ConfigType type_;
    PerfectLinkConfig perfect_link_config_;
    FIFOBroadcastConfig fifo_broadcast_config_;
    LatticeAgreementConfig lattice_agreement_config_;
    Config() = default;
};

#endif