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
    
    // Check config type and run appropriate milestone
    if (config.getType() == ConfigType::PERFECT_LINK) 
    {
      auto pl_config = config.getPerfectLinkConfig();

      // Convert Parser::Host to my types Host
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

      milestone1::PerfectLinkApp app(
          static_cast<uint32_t>(parser.id()),
          hosts,
          pl_config.m,
          pl_config.receiver_id,
          parser.outputPath()
      );
      
      app.run();
      
    //   // Wait for stop signal
    //   while (!SignalHandler::shouldStop()) 
    //   {
    //       std::this_thread::sleep_for(std::chrono::milliseconds(100));
    //   }

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