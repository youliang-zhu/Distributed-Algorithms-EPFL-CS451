#include "signal_handler.hpp"
#include <csignal>

std::atomic<bool> SignalHandler::stop_flag_(false);

void SignalHandler::setup() 
{
    std::signal(SIGTERM, SignalHandler::handleSignal);
    std::signal(SIGINT, SignalHandler::handleSignal);
}

bool SignalHandler::shouldStop() 
{
    return stop_flag_.load();
}

void SignalHandler::handleSignal(int signal) 
{
    stop_flag_.store(true);
    std::signal(SIGTERM, SIG_DFL);
    std::signal(SIGINT, SIG_DFL);
}