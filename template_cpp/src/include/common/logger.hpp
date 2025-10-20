#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

class Logger {
public:
    //构造函数，创建时自动运行
    explicit Logger(const std::string& output_path);
    //析构函数，销毁时自动执行
    ~Logger();

    void logBroadcast(uint32_t seq_number);
    void logDelivery(uint32_t sender_id, uint32_t seq_number);
    void flush();

private:
    std::string output_path_;
    std::vector<std::string> buffer_;
    // 创建锁变量
    std::mutex mtx_;
    
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
};

#endif