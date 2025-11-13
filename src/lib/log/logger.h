//
// Created by user on 2025/11/11.
//

#ifndef LOGGER_H
#define LOGGER_H



// --- Logger.h (模拟) ---
#include <format>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string_view>
#include <chrono>
#include <functional>

#include "../utils/block_queue.h"

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

/// 定义一个日志消息结构体。目的是让格式化操作不在主线程完成，而是由后台线程实现，所以需要将需要日志的内容封装并传递给并发队列。
/// 此时并发队列中存储的就不是格式化后的字符串了，而是带格式化的数据
struct LogMessage {
    // 日志级别
    LogLevel level;
    // 日志消息的精确时间戳
    std::chrono::system_clock::time_point ts;
    // 定义一个对应的格式化操作者，被后台线程调用
    std::function<void(std::ostringstream&)> formatter;
};

/// 一个遵循单例模式的日志库
/// 支持异步写入功能
class Logger {
public:
    /// 获取日志实例**引用**的接口
    /// 我们这里使用的是懒汉模式（只有被调用的时候才进行初始化）
    /// 单例的另一种实现方式是饿汉模式，在刚开始就进行初始化
    static Logger& get_instance();


    /// Logger的配置项
    struct Config {
        /// 日志所在的文件夹
        std::string log_folder;
        /// 异步日志队列的最大长度，使用0就表示同步方式
        size_t max_queue_size = 0;
        /// 每个日志文件的最大行数
        size_t max_lines_per_file = 5000000;
        /// 全局配置，是否关闭日志功能
        bool close_log = false;

        /// 异步模式下，自动刷新的时间间隔（单位：秒）
        /// 设置为0则表示每次写入都刷新（等同于原先的行为）
        int flush_interval_seconds = 3;
    };

    /// 初始化
    void init(const Config &config);

    /// 核心功能：日志
    /// 使用模板和可变参数模板，实现类型安全的格式化
    template<typename... Args>
    void log(LogLevel level, const char* format, Args&&... args);
    /// info
    template<typename... Args>
    void info(const char* format, Args&&... args);
    /// warn
    template<typename... Args>
    void warn(const char* format, Args&&... args);
    /// debug
    template<typename... Args>
    void debug(const char* format, Args&&... args);
    /// error
    template<typename... Args>
    void error(const char* format, Args&&... args);

    /// 停止日志
    void stop();

    // 禁止拷贝和赋值，也是为了实现单例模式
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    // 构造和析构都设置为私有，以实现单例模式
    Logger() = default;
    ~Logger() = default;

    /// 打开新文件的逻辑
    /// 被用于初始化时创建新文件；同时也用于执行实际的日志分片功能
    /// note 它同样假设调用者已经获取了锁
    void open_new_log_file();

    /// 实际的写入文件逻辑，这是一个线程安全的方法
    /// 1. 可以被log直接调用，此时就是同步模式
    /// 2. 也可以被async_write_task()函数间接调用，在m_writer_thread线程运行中，此时就是异步模式
    void write_to_file(const std::string &line);

    /// 执行异步写入日志的线程函数
    void async_write_task();

    /// 带level的格式化字符串，用于同步写入
    template<typename... Args>
    std::string format_log_line(LogLevel level, const char* format, Args&&... args);

    Config m_config;

    // 当异步写入功能开启时，会配置一个指定的队列
    // std::unique_ptr<BlockingQueue<std::string>> m_log_queue;
    // note 优化，将字符串更换为日志消息
    std::unique_ptr<BlockingQueue<LogMessage>> m_log_queue;
    // 如果是异步写入，则专门用于写入日志的线程
    std::unique_ptr<std::thread> m_writer_thread;

    // 文件对象
    std::ofstream m_file;

    // 当前的行数
    unsigned long long m_line_count = 0;

    // 总的行数
    unsigned long long m_total_line = 0;

    // 按照天切分日志，所以需要记录当前系统的天数
    // todo 考虑到本地时钟回拨的情况
    std::chrono::time_point<std::chrono::system_clock, std::chrono::days> m_today {};

    // 记录上一次刷新的时间点，用于周期性刷新
    std::chrono::system_clock::time_point m_last_flush_time;

    // 是否初始化
    std::atomic<bool> m_init;

    // 是否停止
    std::atomic<bool> m_stop;

    // 锁，用于保护文件操作、和其他临界资源
    std::mutex m_mutex;
};

// note log不再亲自执行格式化操作了，而是简单创建LogMessage结构体，并push到并发队列中
template<typename ... Args>
void Logger::log(LogLevel level, const char *format, Args &&...args) {
    // 如果没有启用日志或已经被关闭，则直接退出
    if(m_config.close_log || m_stop.load()) {
        return;
    }
    // // 在锁外格式化日志，允许多线程并行处理
    // std::string line = format_log_line(level, format, std::forward<Args>(args)...);

    // 区分是同步还是异步，同步则当前线程完成，异步则交给阻塞队列
    if(m_config.max_queue_size > 0) {
        // 异步模式：推入队列，push内部是线程安全的
        // 热路径优化，现在不进行直接格式化，而是交给后台线程
        LogMessage msg {.level = level, .ts = std::chrono::system_clock::now()};

        msg.formatter = [=](std::ostringstream& ss) {
            try {
                // vformat在后台线程中被调用
                ss << std::vformat(format, std::make_format_args(args...));
            } catch (const std::format_error& e) {
                ss << " (format error: " << e.what() << ")";
            }
        };
        m_log_queue->push(std::move(msg));
    } else {
        // 同步模式：直接写入文件
        std::string line = format_log_line(level, format, std::forward<Args>(args)...);
        write_to_file(line);
    }
}

template<typename ... Args>
void Logger::info(const char *format, Args &&...args) {
    log(LogLevel::INFO, format, std::forward<Args>(args)...);
}
template<typename ... Args>
void Logger::warn(const char *format, Args &&...args) {
    log(LogLevel::WARN, format, std::forward<Args>(args)...);
}
template<typename ... Args>
void Logger::debug(const char *format, Args &&...args) {
    log(LogLevel::DEBUG, format, std::forward<Args>(args)...);
}
template<typename ... Args>
void Logger::error(const char *format, Args &&...args) {
    log(LogLevel::ERROR, format, std::forward<Args>(args)...);
}

template<typename ... Args>
std::string Logger::format_log_line(LogLevel level, const char *format, Args &&...args) {
    // 获取当前时间
    auto now = std::chrono::system_clock::now();

    // 拼接时间
    std::ostringstream ss;
    ss << std::format("{:%Y-%m-%d %H:%M:%S}", now);
    // ss << std::format("test-time");

    // 获取日志级别字符串
    const char* level_str;
    switch (level) {
        case LogLevel::DEBUG: level_str = " [DEBUG] "; break;
        case LogLevel::INFO:  level_str = " [INFO]  "; break;
        case LogLevel::WARN:  level_str = " [WARN]  "; break;
        case LogLevel::ERROR:
        default:
        level_str = " [ERROR] ";
    }
    ss << level_str;

    // 格式化用户消息
    try {
        // note 使用vformat而非format，是因为format必须要求参数能够在静态编译阶段能够被转化为字符串，如果不就会导致程序崩溃。
        // 而vformat会将参数的类型信息擦除，并在运行时检查，它常用于构建更上层的格式化工具：如日志库、本地化库等
        ss << std::vformat(format, std::make_format_args(args...));
    } catch (const std::format_error& e) {
        ss << " (format error: " << e.what() << ")";
    }
    ss << '\n';

    return ss.str();
}

#endif //LOGGER_H
