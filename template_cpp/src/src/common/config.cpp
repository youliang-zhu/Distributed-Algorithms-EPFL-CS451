#include "config.hpp"
#include <vector>
#include <fstream>
#include <sstream>

Config Config::parse(const std::string& config_path) 
{
    Config config;
    std::ifstream file(config_path);
    
    if (!file.is_open()) 
    {
        return config;
    }
    
    std::string first_line;
    std::getline(file, first_line);
    
    std::istringstream iss(first_line);
    uint32_t first_num, second_num, third_num;
    
    iss >> first_num;
    if (iss >> second_num) 
    {
        // Has at least two numbers
        if (iss >> third_num) 
        {
            // Three numbers: Lattice Agreement "p vs ds"
            config.type_  = ConfigType::LATTICE_AGREEMENT;
            config.lattice_agreement_config_.proposals = first_num;
            config.lattice_agreement_config_.max_values = second_num;
            config.lattice_agreement_config_.distinct_values = third_num;
            
            // Parse proposal sets
            std::string line;
            while (std::getline(file, line)) 
            {
                std::istringstream line_iss(line);
                std::vector<uint32_t> proposal_set;
                uint32_t value;
                while (line_iss >> value) {
                    proposal_set.push_back(value);
                }
                if (!proposal_set.empty()) {
                    config.lattice_agreement_config_.proposal_sets.push_back(proposal_set);
                }
            }
        } 
        else 
        {
            // Two numbers: Perfect Link "m i"
            config.type_ = ConfigType::PERFECT_LINK;
            config.perfect_link_config_.m = first_num;
            config.perfect_link_config_.receiver_id = second_num;
        }
    } 
    else 
    {
        // One number: FIFO Broadcast "m"
        config.type_ = ConfigType::FIFO_BROADCAST;
        config.fifo_broadcast_config_.m = first_num;
    }
    
    file.close();
    return config;
}