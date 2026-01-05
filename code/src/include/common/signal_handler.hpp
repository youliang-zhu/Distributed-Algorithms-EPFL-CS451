#ifndef SIGNAL_HANDLER_HPP
#define SIGNAL_HANDLER_HPP

#include <atomic>

class SignalHandler {
public:
    static void setup();
    static bool shouldStop();

private:
    static std::atomic<bool> stop_flag_;
    static void handleSignal(int signal);
    
    // Prevent instantiation
    SignalHandler() = delete;
    ~SignalHandler() = delete;
    SignalHandler(const SignalHandler&) = delete;
    SignalHandler& operator=(const SignalHandler&) = delete;
};

#endif