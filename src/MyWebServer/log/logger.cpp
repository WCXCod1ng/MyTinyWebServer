//
// Created by user on 2025/11/11.
//

#include "logger.h"
#include <filesystem>
#include <iostream>

#include "console_sink.h"
#include "file_sink.h"

namespace fs = std::filesystem;
/// 在C++11之前，懒汉模式必须要进行双锁检测
/// 但是在C++11之后，我们可以通过创建一个静态局部变量来实现懒汉模式
Logger& Logger::get_instance() {
    static Logger logger;
    return logger;
}

void Logger::init(const Config &config) {
    if(config.flush_interval_seconds < 0) {
        throw std::runtime_error("invalid flush_interval_seconds");
    }
    // 不开启异步模式，就不能开启周期flush
    if(config.max_queue_size == 0 && config.flush_interval_seconds > 0) {
        throw std::runtime_error("max_queue_size == 0 and flush_interval_seconds > 0 can not be all true");
    }

    // 上锁
    std::lock_guard<std::mutex> lock(m_mutex);
    if(m_init.load()) {
        return;
    }
    m_init.store(true);

    // 保存配置
    m_config = config;

    // 设置日志级别
    m_active_level.store(m_config.level);

    // 初始化sinks
    m_sinks.clear();

    // 控制台输出
    if(config.enable_console_sink) {
        m_sinks.push_back(std::make_shared<ConsoleSink>());
    }

    // 日志文件输出
    if(!config.log_folder.empty()) {
        m_sinks.push_back(std::make_shared<FileSink>(
            config.log_folder,
            config.max_lines_per_file,
            config.is_override
        ));
    }

    // 如果没有任何Sink被创建，则无需继续
    if (m_sinks.empty()) {
        m_init.store(false);
        return;
    }

    // 如果开启异步模式，则创建阻塞队列和writer_thread
    if(m_config.max_queue_size > 0) {
        // 初始化周期性刷新的成员变量
        m_last_flush_time = std::chrono::system_clock::now();
        m_log_queue = std::make_unique<BlockingQueue<LogMessage>>(m_config.max_queue_size);
        m_writer_thread = std::make_unique<std::thread>([this] {
            async_write_task();
        });
    }

    // 初始化其他参数
    m_stop.store(false);
    m_init.store(true);
} // 自动解锁


void Logger::set_level(LogLevel level) {
    m_active_level.store(level);
}

LogLevel Logger::get_level() const {
    return m_active_level.load();
}

void Logger::stop() {
    // 上锁
    std::unique_lock<std::mutex> lock(m_mutex);

    // 幂等性
    if(!m_init || m_stop) {
        return;
    }
    m_stop.store(true);

    // 如果开启异步写入功能，则关闭阻塞队列（这里可以不判断，只要不为nullptr就进行关闭）
    // fixme 依赖阻塞队列自己实现的析构函数进行关闭
    // 关闭阻塞队列
    if(m_log_queue != nullptr) {
        m_log_queue->close();
    }

    // 解锁，让后台线程完成最后的写入
    lock.unlock();

    // 等待写入线程关闭，之后自行退出
    if(m_writer_thread != nullptr && m_writer_thread->joinable()) {
        m_writer_thread->join();
        m_writer_thread = nullptr;
    }

    for(const auto& sink :m_sinks) {
        sink->flush();
    }
    // 清理Sinks。这将调用FileSink等的析构函数，从而安全地关闭文件句柄。
    m_sinks.clear();

    m_init.store(false);
}

void Logger::async_write_task() {
    // 创建一个可复用的字符串流
    std::ostringstream ss;
    // 我们实际上是通过调用同步函数来实现异步写的
    // 持续从阻塞队列中获取日志项，并且调用同步函数写入文件
    while(true) {
        std::optional<LogMessage> entry;
        if(m_config.flush_interval_seconds == 0) {
            // 如果关闭周期检查，则调用无限阻塞的pop
            entry = m_log_queue->pop();
        } else {
            // 否则使用带超时的pop，等待1秒或直到有新消息，主要是为了防止没有消息而导致线程被阻塞，从而影响刷新
            // todo 根据上一次刷新时间和刷新周期动态计算等待时间
            entry = m_log_queue->pop_for(std::chrono::seconds(1));
        }

        // 如果成功获取到日志，则处理它
        if(entry) {
            // 格式化消息
            const auto& formatted_string = format_log_line(entry.value());
            // 分发给所有Sink
            for(const auto& sink : m_sinks) {
                sink->log(formatted_string);
            }

            // note 处理“异步写、同步刷新”的情况，必须在这里立即刷新
            if(m_config.flush_interval_seconds == 0) {
                for(const auto& sink : m_sinks) {
                    sink->flush();
                }
            }
        } else {
            // 为空有两种情况：1. 超时了；2. 队列已经关闭且为空，现成可以退出了
            // note 不能只根据是否关闭来判断，而是要根据日志队列是否被关闭，为了防止stop和后台log方法并发执行时的竞争问题
            if(m_log_queue->is_closed()) {
                break;
            }
        }

        // 如果配置了周期刷新，则需要再后台线程中检查是否需要flush（因为write_to_file已经不会进行刷新了）
        // note 处理“异步写、异步刷新”的情况；而且这里不能在上面的if语句内，一个重要的原因是为了包含住“同步写、异步刷新”的情况
        if(m_config.flush_interval_seconds > 0) {
            // 检查是否超过周期
            auto now = std::chrono::system_clock::now();
            if(auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_last_flush_time); elapsed.count() >= m_config.flush_interval_seconds) {
                // // 注意刷新操作需要加锁保护
                // std::lock_guard<std::mutex> lock(m_mutex);
                // if(m_file.is_open()) {
                //     m_file.flush();
                // }
                // 分发给所有Sink
                for(const auto& sink : m_sinks) {
                    sink->flush();
                }
                // 更新刷新时间
                m_last_flush_time = now;
            }
        }
    }

    // 队列关闭后，循环退出前，执行最后一次刷新
    for(const auto& sink : m_sinks) {
        sink->flush();
    }
}


std::string Logger::format_log_line(const LogMessage& msg) {
    const auto level = msg.level;
    const auto& loc = msg.location;

    std::ostringstream ss;

    // 时间
    auto now = std::chrono::system_clock::now();

    // 日志级别
    const char* level_str;
    switch (level) {
        case LogLevel::DEBUG: level_str = " [DEBUG] "; break;
        case LogLevel::INFO:  level_str = " [INFO]  "; break;
        case LogLevel::WARN:  level_str = " [WARN]  "; break;
        case LogLevel::ERROR:
            default:
            level_str = " [ERROR] ";
    }

    // 源码位置信息
    std::string filepath = std::filesystem::path(loc.file_name()).string();

    // 格式化日志头
    ss << std::format("{} {:%Y-%m-%d %H:%M:%S}", level_str, now);

    ss << " [" << msg.thread_name << "] "; // 线程名

    ss << std::format("{} {} {}:{} ", filepath, loc.function_name(), loc.line(), loc.column());

    // 格式化用户消息
    try {
        // 调用log传入的formatter
        msg.formatter(ss);
    } catch (const std::format_error& e) {
        ss << " (format error: " << e.what() << ")";
    }
    ss << '\n';

    return ss.str();
}
