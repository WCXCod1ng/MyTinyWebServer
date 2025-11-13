//
// Created by user on 2025/11/11.
//

#include "logger.h"
#include <filesystem>
#include <iostream>

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

    // 目录不存在则尝试创建目录
    fs::path log_folder = config.log_folder;
    if(!log_folder.empty() && !fs::exists(log_folder)) {
        fs::create_directories(log_folder);
    }
    // 保存配置
    m_config = config;

    // 如果开启异步模式，则创建阻塞队列和writer_thread
    if(m_config.max_queue_size > 0) {
        // 初始化周期性刷新的成员变量
        m_last_flush_time = std::chrono::system_clock::now();
        m_log_queue = std::make_unique<BlockingQueue<LogMessage>>(m_config.max_queue_size);
        m_writer_thread = std::make_unique<std::thread>([this] {
            async_write_task();
        });
    }

    // 初始化
    m_line_count = 0;
    m_total_line = 0;
    m_today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());

    // 打开日志文件
    open_new_log_file();

} // 自动解锁


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

    // 关闭文件流
    // 注意，需要加锁
    lock.lock();
    if(m_file.is_open()) {
        // 关闭前执行最后一次刷新，确保所有的日志都已经写入磁盘
        m_file.flush();
        m_file.close();
    }
}


void Logger::open_new_log_file() {
    // 关闭旧文件
    if(m_file.is_open()) {
        m_file.close();
    }

    // 生成新文件名
    // 文件名的格式是：log_2025_11_12_000000（6位编号足以）
    const auto now = std::chrono::system_clock::now();
    fs::path log_folder_path = m_config.log_folder;
    // 格式化文件名，用户传入的文件名之后添加后缀。
    std::string file_name = std::format("log_{0:%Y_%m_%d}_{1:06d}.log", now, m_line_count / m_config.max_lines_per_file);

    // 生成新文件路径
    // 注意，由于path重载了“/”运算符，所以可以这样写
    fs::path new_path = log_folder_path / file_name;

    // 创建文件
    m_file.open(new_path, std::ios::app);
    if(!m_file.is_open()) {
        std::cerr << "Failed to open log file" << new_path << std::endl;
    }
}

void Logger::write_to_file(const std::string &line) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 首先要检查是否需要日志切分（日志切分的依据是：天数不同则切分、行数超过单个文件最大行数也要切分）
    // 获取当前系统时间点
    const auto now = std::chrono::system_clock::now();
    // 转换为“天”为单位的时间点
    const auto today = floor<std::chrono::days>(now);

    // 判断是否需要切分
    if(m_file.is_open() && (today != m_today) || (m_line_count % m_config.max_lines_per_file == 0 && m_line_count > 0)) {
        open_new_log_file();
    }

    // 执行实际的写入操作
    if(m_file.is_open()) {
        m_file << line;
        // 这里我们可以根据配置确定是否立即刷新，如果启用了周期刷新，那么flush操作由后台线程完成；饭后则由write_to_file完成
        if(m_config.flush_interval_seconds == 0) {
            m_file.flush();
        }
        ++m_total_line; // 总行数+1

        // 更新天数和当前的行数
        if(m_today != today) {
            m_today = today;
            m_line_count = 0;
        } else {
            ++m_line_count;
        }
    }
}

void Logger::async_write_task() {
    // 创建一个可复用的字符串流
    std::ostringstream ss;
    // 我们实际上是通过调用同步函数来实现异步写的
    // 持续从阻塞队列中获取日志项，并且调用同步函数写入文件
    while(!m_stop.load()) {
        std::optional<LogMessage> entry;
        if(m_config.flush_interval_seconds == 0) {
            // 如果关闭周期检查，则调用无限阻塞的pop
            entry = m_log_queue->pop();
        } else {
            // 否则使用带超时的pop，等待1秒或直到有新消息
            entry = m_log_queue->pop_for(std::chrono::seconds(1));
        }


        // 如果成功获取到日志，则处理它
        if(entry) {
            LogMessage& msg = entry.value();
            // 格式化日志头（时间、级别）
            ss << std::format("{:%Y-%m-%d %H:%M:%S}", msg.ts);

            const char* level_str;
            switch (msg.level) {
                case LogLevel::DEBUG: level_str = " [DEBUG] "; break;
                case LogLevel::INFO:  level_str = " [INFO]  "; break;
                case LogLevel::WARN:  level_str = " [WARN]  "; break;
                case LogLevel::ERROR:
                    default: level_str = " [ERROR] ";
            }
            ss << level_str;

            // 执行之前被封装的格式化操作（与用户传入的格式化字符串）
            msg.formatter(ss);

            // end a line
            ss << '\n';

            // 获取完整日志字符串并写入文件
            write_to_file(ss.str());

            // 重置流以备下次使用，这是一个重要的性能优化
            ss.str("");
            ss.clear();
        }

        // 如果配置了周期刷新，则需要再后台线程中检查是否需要flush（因为write_to_file已经不会进行刷新了）
        if(m_config.flush_interval_seconds > 0) {
            // 检查是否超过周期
            auto now = std::chrono::system_clock::now();
            if(auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_last_flush_time); elapsed.count() >= m_config.flush_interval_seconds) {
                // 注意刷新操作需要加锁保护
                std::lock_guard<std::mutex> lock(m_mutex);
                if(m_file.is_open()) {
                    m_file.flush();
                }
                // 更新刷新时间
                m_last_flush_time = now;
            }
        }
    }

    // 队列关闭后，循环退出前，执行最后一次刷新
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open()) {
        m_file.flush();
    }
}
