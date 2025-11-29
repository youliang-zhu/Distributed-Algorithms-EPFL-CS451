#include <iostream>
#include <thread>
#include <chrono>
#include <iostream> 
#include "parser.hpp"
#include "common/signal_handler.hpp"
#include "common/config.hpp"
#include "perfectlink/perfect_link_app.hpp"
#include "fifobroadcast/fifo_broadcast_app.hpp"

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
      
      //如果是sender在run结束后已经确认所有acked，可以直接shutdown退出
      if (app.isSender())
      {
          app.shutdown();
      } 
      else 
      //作为receiver，是不知道还有多少消息需要收的，只能等收到停止信号后再shutdown
      {
          int wait_count = 0;
          //SignalHandler::setup()会设置stop_flag_，捕捉SIGINT信号，这里每100ms检查一次是否收到停止信号
          while (!SignalHandler::shouldStop()) 
          {
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          app.shutdown();
      };
    }
    else if (config.getType() == ConfigType::FIFO_BROADCAST)
    {
      auto fifo_config = config.getFIFOBroadcastConfig();

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

      milestone2::FIFOBroadcastApp app(
          static_cast<uint32_t>(parser.id()),
          hosts,
          fifo_config.m,
          parser.outputPath()
      );
      
      app.run();
      
      while (!SignalHandler::shouldStop()) 
      {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      app.shutdown();
    }
    else 
    {
      std::cerr << "Unknown config type\n";
      return 1;
    }
    
    return 0;
}