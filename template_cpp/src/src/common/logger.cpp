#include "logger.hpp"
#include <fstream>

Logger::Logger(const std::string& output_path) 
    : output_path_(output_path) 
{
    buffer_.reserve(10000);
}

Logger::~Logger() 
{
    flush();
}

void Logger::logBroadcast(uint32_t seq_number) 
{
    //创建一个名为 lock 的临时对象，它会在构造时自动锁住 mtx_，在销毁时自动解锁
    //lock_guard 的生命周期和 logBroadcast 函数的局部作用域绑定。当函数执行完毕并离开作用域，lock_guard 自动释放锁
    std::lock_guard<std::mutex> lock(mtx_);
    buffer_.push_back("b " + std::to_string(seq_number));
    if (buffer_.size() >= FLUSH_THRESHOLD) 
    {
        flushInternal();
    }
}

void Logger::logDelivery(uint32_t sender_id, uint32_t seq_number) 
{
    std::lock_guard<std::mutex> lock(mtx_);
    buffer_.push_back("d " + std::to_string(sender_id) + " " + std::to_string(seq_number));
    if (buffer_.size() >= FLUSH_THRESHOLD) 
    {
        flushInternal();
    }
}

void Logger::flush() 
{
    std::lock_guard<std::mutex> lock(mtx_);
    flushInternal();
}

void Logger::flushInternal() 
{
    if (buffer_.empty()) 
    {
        return;
    }
    
    // 使用追加模式打开文件
    std::ofstream file(output_path_, std::ios::app);
    if (!file.is_open()) 
    {
        return;
    }
    
    for (const std::string& line : buffer_) 
    {
        file << line << "\n";
    }
    
    file.close();
    buffer_.clear();
}