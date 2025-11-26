//
// Created by user on 2025/11/18.
//

#ifndef CONSOLE_SINK_H
#define CONSOLE_SINK_H
#include <mutex>

#include "sink.h"

class ConsoleSink final : public ISink{
public:
    ConsoleSink() = default;

    // 禁止拷贝和赋值
    ConsoleSink(const ConsoleSink&) = delete;
    ConsoleSink& operator=(const ConsoleSink&) = delete;

    void log(const std::string &formatted_message) override {
        // 上锁后直接利用cout输出即可，它不存在日志分块等问题
        std::lock_guard<std::mutex> lock(m_mutex);
        std::cout.write(formatted_message.c_str(), formatted_message.length());
    }

    void flush() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::cout.flush();
    }

private:
    // 保护std::cout
    std::mutex m_mutex;
};

#endif //CONSOLE_SINK_H
