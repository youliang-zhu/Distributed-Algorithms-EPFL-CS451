#include <iostream>
#include <thread>
#include <chrono>
#include <iostream> 
#include "parser.hpp"
#include "common/signal_handler.hpp"
#include "common/config.hpp"
#include "perfectlink/perfect_link_app.hpp"

int main(int argc, char** argv) 
{
    Parser parser(argc, argv);
    parser.parse();
    
    SignalHandler::setup();
    Config config = Config::parse(parser.configPath());
    
    if (config.getType() == ConfigType::PERFECT_LINK) 
    {
      auto pl_config = config.getPerfectLinkConfig();

      // HOST convert
      auto parser_hosts = parser.hosts();
      std::vector<Host> hosts;
      for (const auto& ph : parser_hosts) 
      {
          hosts.emplace_back(
              static_cast<uint32_t>(ph.id),
              ph.ipReadable(),
              ph.portReadable()
          );
      }

      // create pl class, input id and host parse from command, load m and id from config file
      milestone1::PerfectLinkApp app(
          static_cast<uint32_t>(parser.id()),
          hosts,
          pl_config.m,
          pl_config.receiver_id,
          parser.outputPath()
      );
      
      app.run();
      
      std::cout << "[DEBUG] Entering signal wait loop..." << std::endl;
      int wait_count = 0;
      while (!SignalHandler::shouldStop()) 
      {
          wait_count++;
          if (wait_count % 10 == 1) {  // Print every second (10 * 100ms)
              std::cout << "[DEBUG] Signal wait loop iteration #" << wait_count << std::endl;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      std::cout << "[DEBUG] Signal received, exiting wait loop after " << wait_count << " iterations" << std::endl;
      std::cout << "Process " << parser.id() << ": Task completed, shutting down..." << std::endl;
      app.shutdown();
        
    }
    else 
    {
      std::cerr << "Unknown config type\n";
      return 1;
    }
    
    return 0;
}