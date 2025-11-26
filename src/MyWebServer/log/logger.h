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
#include <source_location>
#include <filesystem>
#include <thread>
#include <pthread.h>

#include "sink.h"
#include "../utils/block_queue.h"
#include "base/utils.h"

enum class LogLevel {
    DEBUG = 0, // 赋予确定的数值，便于比较
    INFO = 1,
    WARN = 2,
    ERROR = 3,
    NONE = 4, // 新增一个类别，如果设置为了它，表示关闭所有日志
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
    // 新增成员：存储日志调用点处的源码位置信息
    std::source_location location;
    // 新增成员，存储调用日志的线程的线程ID
    std::thread::id thread_id;
    // 存储线程名
    std::string thread_name;
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

        // 是否开启控制台输出
        bool enable_console_sink = true;

        /// 异步模式下，自动刷新的时间间隔（单位：秒）
        /// 设置为0则表示每次写入都刷新（等同于原先的行为）
        int flush_interval_seconds = 3;

        /// 日志等级，默认为超过INFO的都显示（也即除了DEBUG都显示）
        LogLevel level = LogLevel::INFO;

        /// 是否覆盖日志
        bool is_override = false;
    };

    /// 初始化
    void init(const Config &config);

    /// 设置日志级别
    void set_level(LogLevel level);

    /// 获取日志级别
    LogLevel get_level() const;

    /// 核心功能：日志
    /// 使用模板和可变参数模板，实现类型安全的格式化
    template<typename... Args>
    void log(
        const std::source_location& loc, // 支持传入源文件的信息
        LogLevel level,
        const char* format,
        Args&&... args);
    /// info
    template<typename... Args>
    void info(const std::source_location& loc, const char* format, Args&&... args);
    /// warn
    template<typename... Args>
    void warn(const std::source_location& loc, const char* format, Args&&... args);
    /// debug
    template<typename... Args>
    void debug(const std::source_location& loc, const char* format, Args&&... args);
    /// error
    template<typename... Args>
    void error(const std::source_location& loc, const char* format, Args&&... args);

    /// 停止日志
    void stop();

    // 禁止拷贝和赋值，也是为了实现单例模式
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    // 构造和析构都设置为私有，以实现单例模式
    Logger() = default;
    ~Logger() {
        this->stop();
    };

    /// 执行异步写入日志的线程函数
    void async_write_task();

    /// 用于将msg消息格式化为字符串，既用于同步，也用于异步
    static std::string format_log_line(const LogMessage& msg);

    Config m_config;

    // 当异步写入功能开启时，会配置一个指定的队列
    // std::unique_ptr<BlockingQueue<std::string>> m_log_queue;
    // note 优化，将字符串更换为日志消息
    std::unique_ptr<BlockingQueue<LogMessage>> m_log_queue;
    // 如果是异步写入，则专门用于写入日志的线程
    std::unique_ptr<std::thread> m_writer_thread;

    // 持有所有Sink的容器
    std::vector<std::shared_ptr<ISink>> m_sinks;

    // 记录上一次刷新的时间点，用于周期性刷新
    std::chrono::system_clock::time_point m_last_flush_time;

    // 是否初始化
    std::atomic<bool> m_init {false};

    // 是否停止
    std::atomic<bool> m_stop {false};

    // 增加日志级别以符合优先级显示
    std::atomic<LogLevel> m_active_level {LogLevel::INFO};

    // 锁，用于保护文件操作、和其他临界资源
    std::mutex m_mutex;
};

// note log不再亲自执行格式化操作了，而是简单创建LogMessage结构体，并push到并发队列中
template<typename ... Args>
void Logger::log(
    const std::source_location& loc,
    LogLevel level,
    const char *format,
    Args &&...args) { // 在实现中接受该参数，函数内部可以直接使用它
    // 日志过滤，以提供不同优先级的显示
    if(level < m_active_level.load(std::memory_order_relaxed)) {
        return;
    }

    // 如果没有启用日志或已经被关闭，则直接退出
    if(m_config.close_log || m_stop.load()) {
        return;
    }
    // // 在锁外格式化日志，允许多线程并行处理
    // std::string line = format_log_line(level, format, std::forward<Args>(args)...);

    // 构建日志消息体
    LogMessage msg {
        .level = level,
        .ts = std::chrono::system_clock::now(),
        .formatter = [=](std::ostringstream& ss) {
            try {
                // note 使用vformat而非format，是因为format必须要求参数能够在静态编译阶段能够被转化为字符串，如果不就会导致程序崩溃。
                // 而vformat会将参数的类型信息擦除，并在运行时检查，它常用于构建更上层的格式化工具：如日志库、本地化库等
                ss << std::vformat(format, std::make_format_args(args...));
            } catch (const std::format_error& e) {
                ss << " (format error: " << e.what() << ")";
            }
        },
        .location = loc,
        .thread_id = std::this_thread::get_id(),
        .thread_name = getCurrentThreadName()
    };

    // 区分是同步还是异步，同步则当前线程完成，异步则交给阻塞队列
    if(m_config.max_queue_size > 0) {
        // 异步模式：推入队列，push内部是线程安全的
        // 热路径优化，现在不进行直接格式化，而是交给后台线程
        m_log_queue->push(std::move(msg));
    } else {
        // 同步模式：直接写入文件
        const std::string formatted_string = format_log_line(msg);
        // write_to_file(line); // 现在不是由log写入，而是调用不同Sink的函数写入
        for(const auto& sink : m_sinks) {
            sink->log(formatted_string);
        }
        // note 处理“同步写、同步刷新”的情况
        // 对于异步模式会留到async_write任务中完成，对于同步模式则在这里立即刷新（因为将来就没有机会flush了）
        for(const auto& sink : m_sinks) {
            sink->flush();
        }
    }
}

template<typename ... Args>
void Logger::info(const std::source_location& loc, const char *format, Args &&...args) {
    log(loc, LogLevel::INFO, format, std::forward<Args>(args)...);
}
template<typename ... Args>
void Logger::warn(const std::source_location& loc, const char *format, Args &&...args) {
    log(loc, LogLevel::WARN, format, std::forward<Args>(args)...);
}
template<typename ... Args>
void Logger::debug(const std::source_location& loc, const char *format, Args &&...args) {
    log(loc, LogLevel::DEBUG, format, std::forward<Args>(args)...);
}
template<typename ... Args>
void Logger::error(const std::source_location& loc, const char *format, Args &&...args) {
    log(loc, LogLevel::ERROR, format, std::forward<Args>(args)...);
}

#define LOG(level, ...) \
    do { \
        Logger::get_instance().log(std::source_location::current(), level, __VA_ARGS__); \
    } while(0)

#define LOG_INFO(...) \
     do { \
         Logger::get_instance().info(std::source_location::current(), __VA_ARGS__); \
     } while(0)

#define LOG_WARN(...) \
    do { \
        Logger::get_instance().warn(std::source_location::current(), __VA_ARGS__); \
    } while(0)


#define LOG_DEBUG(...) \
    do { \
        Logger::get_instance().debug(std::source_location::current(), __VA_ARGS__); \
    } while(0)

#define LOG_ERROR(...) \
    do { \
        Logger::get_instance().error(std::source_location::current(), __VA_ARGS__); \
    } while(0)


#endif //LOGGER_H
