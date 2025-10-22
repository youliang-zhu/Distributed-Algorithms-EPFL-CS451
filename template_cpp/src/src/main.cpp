#include <iostream>
#include <thread>
#include <chrono>
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
      milestone1::PerfectLinkApp app(
          parser.id(),
          parser.hosts(),
          pl_config.m,
          pl_config.receiver_id,
          parser.outputPath()
      );
      
      app.run();
      
      // Wait for stop signal
      while (!SignalHandler::shouldStop()) 
      {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      
      // Shutdown and flush logs
      app.shutdown();
        
    }
    else 
    {
      std::cerr << "Unknown config type\n";
      return 1;
    }
    
    return 0;
}