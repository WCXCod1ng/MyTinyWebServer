//
// Created by user on 2025/11/18.
//
#include "file_sink.h"
#include <filesystem>
#include <format>
#include <iostream>
#include <chrono>

namespace fs = std::filesystem;


FileSink::FileSink(std::string log_folder, size_t max_lines_per_file, bool is_override)
    : m_log_folder(std::move(log_folder)),
      m_max_lines_per_file(max_lines_per_file > 0 ? max_lines_per_file : 1),
      m_is_override(is_override){
    try {
        if (!fs::exists(m_log_folder)) {
            fs::create_directories(m_log_folder);
        }
    } catch (const fs::filesystem_error& e) {
        // 如果目录创建失败，这是一个严重错误，抛出异常
        throw std::runtime_error("Failed to create log directory: " + m_log_folder + " (" + e.what() + ")");
    }

    m_today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    // 一开始要创建一个文件
    open_new_log_file();
}

FileSink::~FileSink() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 确保关闭文件前数据被存储到磁盘
    if (m_file_stream.is_open()) {
        m_file_stream.flush();
        m_file_stream.close();
    }
}

void FileSink::log(const std::string &formatted_message) {
    // lock
    std::lock_guard<std::mutex> lock(m_mutex);
    // 首先要检查是否需要日志切分（日志切分的依据是：天数不同则切分、行数超过单个文件最大行数也要切分）
    // 获取当前系统时间点
    const auto now = std::chrono::system_clock::now();
    // 转换为“天”为单位的时间点
    const auto today = floor<std::chrono::days>(now);

    // 判断是否需要切分
    if(m_file_stream.is_open() && (today != m_today) || m_current_lines_in_file >= m_max_lines_per_file) {
        open_new_log_file();
        // 更新天数与当前文件的行数
        m_today = today;
        m_current_lines_in_file = 0;
    }

    // 执行实际的写入操作
    if(m_file_stream.is_open()) {
        m_file_stream.write(formatted_message.c_str(), formatted_message.length());
        // 更新当前行数和总行数
        ++m_current_lines_in_file;
        ++m_total_lines;
    }
}

void FileSink::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file_stream.is_open()) {
        m_file_stream.flush();
    }
}

void FileSink::open_new_log_file() {
    // 关闭旧文件
    if(m_file_stream.is_open()) {
        m_file_stream.close();
    }

    // 生成新文件名
    // 文件名的格式是：log_2025_11_12_000000（6位编号足以）
    const auto now = std::chrono::system_clock::now();
    fs::path log_folder_path = m_log_folder;
    // 格式化文件名，用户传入的文件名之后添加后缀。
    std::string file_name = std::format("log_{0:%Y_%m_%d}_{1:06d}.log", now, m_total_lines / m_max_lines_per_file);

    // 生成新文件路径
    // 注意，由于path重载了“/”运算符，所以可以这样写
    fs::path new_path = log_folder_path / file_name;

    // 创建文件
    // 简单配置日志写入时是否覆盖旧日志
    if(m_is_override) {
        m_file_stream.open(new_path, std::ios::out | std::ios::trunc);
    } else {
        m_file_stream.open(new_path, std::ios::app);
    }
    if(!m_file_stream.is_open()) {
        std::cerr << "Failed to open log file" << new_path << std::endl;
    }
}
