//
// Created by user on 2025/11/18.
//

#ifndef FILE_SINK_H
#define FILE_SINK_H
#include <fstream>
#include <mutex>

#include "sink.h"

class FileSink final : public ISink{
public:

    /**
     * @brief 构造函数。
     * @param log_folder 日志文件存放的目录。
     * @param max_lines_per_file 每个日志文件的最大行数，超过后会自动切分。
     * @param is_override 是否在启动时覆盖同名日志文件。
     */
    FileSink(std::string log_folder, size_t max_lines_per_file, bool is_override);

    // 禁止拷贝和赋值
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    // 在析构时确保文件被关闭和刷新
    ~FileSink() override;

    void log(const std::string& formatted_message) override;
    void flush() override;

private:
    /**
     * @brief 打开一个新的日志文件，用于初始化或日志切分。
     * 打开新文件的逻辑
     * 被用于初始化时创建新文件；同时也用于执行实际的日志分片功能
     * note 它同样假设调用者已经获取了锁
     */
    void open_new_log_file();

private:
    /// 保护文件写操作的互斥锁
    std::mutex m_mutex;
    /// 日志所在的文件夹
    std::string m_log_folder;

    /// 每个日志文件的最大行数
    size_t m_max_lines_per_file;

    /// 日志文件打开方式：
    /// 1. true表示清空原来文件并重新开始；
    /// 2. false表示追加模式
    bool m_is_override;

    /// 日志文件输出流
    std::ofstream m_file_stream;

    /// 当前的行数
    size_t m_current_lines_in_file = 0;

    /// 总函数
    size_t m_total_lines = 0;

    /// 按照天切分日志，所以需要记录当前系统的天数
    /// todo 考虑到本地时钟回拨的情况
    std::chrono::time_point<std::chrono::system_clock, std::chrono::days> m_today {};

};

#endif //FILE_SINK_H
