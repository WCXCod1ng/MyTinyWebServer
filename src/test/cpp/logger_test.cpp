#include "gtest/gtest.h"
#include "../../MyTinyWebServer/log/logger.h" // 你的日志头文件
#include <filesystem>
#include <fstream>
#include <vector>
#include <thread>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 使用一个基于当前进程ID和时间的唯一目录名，防止并行测试时冲突
        test_log_dir = "/tmp/logger_test_" + std::to_string(getpid()) + "_" +
                        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        fs::create_directories(test_log_dir);
    }

    void TearDown() override {
        // 确保在删除目录前，Logger实例已停止并关闭所有文件句柄
        // 如果Logger是单例，确保它的生命周期能在这里结束
        Logger::get_instance().stop();
        fs::remove_all(test_log_dir);
    }

    // 辅助函数：计算一个目录下有多少个文件
    size_t count_files_in_dir(const fs::path& dir) {
        if (!fs::exists(dir)) return 0;
        return std::distance(fs::directory_iterator(dir), fs::directory_iterator{});
    }

    // 辅助函数：读取目录下所有文件的所有行
    std::vector<std::string> read_all_lines_from_dir(const fs::path& dir) {
        std::vector<std::string> all_lines;
        if (!fs::exists(dir)) return all_lines;

        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                std::ifstream ifs(entry.path());
                std::string line;
                while (std::getline(ifs, line)) {
                    all_lines.push_back(line);
                }
            }
        }
        return all_lines;
    }

    fs::path test_log_dir;
};

// ===================================================================
// 功能性测试 (已更新)
// ===================================================================

TEST_F(LoggerTest, SyncMode_WritesToCorrectFolder) {
    Logger::Config config;
    // *** 修改点: 配置现在是文件夹路径 ***
    config.log_folder = test_log_dir.string();
    config.max_queue_size = 0; // 同步模式
    config.flush_interval_seconds = 0; // 每次写入都刷新
    config.enable_console_sink = true;
    config.is_override = true;

    Logger& log = Logger::get_instance();
    log.init(config);

    LOG_INFO("Hello, Sync Logger!");

    // *** 修改点: 验证文件夹下是否生成了文件 ***
    ASSERT_EQ(count_files_in_dir(test_log_dir), 1) << "Log file was not created in the specified folder.";

    // *** 修改点: 读取文件夹下的所有日志内容进行验证 ***
    auto lines = read_all_lines_from_dir(test_log_dir);
    ASSERT_EQ(lines.size(), 1);
    ASSERT_NE(lines[0].find("Hello, Sync Logger!"), std::string::npos);
}

TEST_F(LoggerTest, AsyncMode_StopFlushesQueue) {
    Logger::Config config;
    config.log_folder = test_log_dir.string();
    config.max_queue_size = 10; // 异步模式
    config.flush_interval_seconds = 0; // 每次写入都刷新
    config.enable_console_sink = true;
    config.is_override = true;

    Logger& log = Logger::get_instance();
    log.init(config);

    LOG_INFO("Testing async write.");

    // 调用stop来确保队列被清空
    Logger::get_instance().stop();

    // 验证逻辑与同步测试相同
    ASSERT_EQ(count_files_in_dir(test_log_dir), 1);
    auto lines = read_all_lines_from_dir(test_log_dir);
    ASSERT_EQ(lines.size(), 1);
    ASSERT_NE(lines[0].find("Testing async write."), std::string::npos);
}

TEST_F(LoggerTest, FileSplitting_ByLineCount) {
    Logger::Config config;
    config.log_folder = test_log_dir.string();
    config.max_lines_per_file = 10;
    config.max_queue_size = 0; // 同步模式以简化测试
    config.flush_interval_seconds = 0; // 每次写入都刷新
    config.enable_console_sink = true;
    config.is_override = true;

    Logger& log = Logger::get_instance();
    log.init(config);

    // 写入15行日志
    for (int i = 0; i < 15; ++i) {
        LOG_INFO("Line number {}", i);
    }

    // *** 修改点: 验证文件夹下是否生成了两个文件 ***
    ASSERT_EQ(count_files_in_dir(test_log_dir), 2) << "Log splitting did not produce two files.";

    // *** 修改点: 读取所有日志内容验证总行数 ***
    auto all_lines = read_all_lines_from_dir(test_log_dir);
    ASSERT_EQ(all_lines.size(), 15);
}

// ===================================================================
// 性能与并发测试 (已更新)
// ===================================================================

TEST_F(LoggerTest, AsyncMode_MultipleThreads_NoDataLoss) {
    Logger::Config config;
    config.log_folder = test_log_dir.string();
    config.max_queue_size = 100000;
    config.max_lines_per_file = 5000; // 设置一个较小的行数，强制在测试中发生文件切分
    config.flush_interval_seconds = 0; // 每次写入都刷新
    config.enable_console_sink = true;
    config.is_override = true;

    Logger& log = Logger::get_instance();
    log.init(config);

    const int num_threads = 10;
    const int messages_per_thread = 1000;
    const int total_messages = num_threads * messages_per_thread;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&log, i, messages_per_thread]() {
            for (int j = 0; j < messages_per_thread; ++j) {
                LOG_INFO("Thread {} writing message {}", i, j);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::this_thread::sleep_for(std::chrono::seconds(10));

    // 停止logger，确保所有日志都写入磁盘
    Logger::get_instance().stop();

    // *** 修改点: 验证逻辑不变，但现在更强大，因为它会遍历所有切分后的文件 ***
    auto all_lines_read = read_all_lines_from_dir(test_log_dir);
    ASSERT_EQ(all_lines_read.size(), total_messages);

    // 可选的额外验证：确保文件数量是正确的
    // total_messages / max_lines_per_file + (total_messages % max_lines_per_file != 0)
    size_t expected_file_count = (total_messages + config.max_lines_per_file - 1) / config.max_lines_per_file;
    ASSERT_EQ(count_files_in_dir(test_log_dir), expected_file_count);
}


/**
 * @brief 测试核心功能：日志在刷新间隔到达后才被写入文件
 */
TEST_F(LoggerTest, FlushHappensAfterInterval) {
    // 1. Arrange: 配置一个2秒的刷新间隔
    Logger::Config config;
    config.log_folder = test_log_dir.string();
    config.max_queue_size = 1024; // 必须是异步模式
    config.flush_interval_seconds = 2; // 设置为一个非零的值，表示启用周期性刷新
    config.enable_console_sink = true;
    config.is_override = true;

    Logger::get_instance().init(config);

    // 2. Act & Assert Phase 1: 立即检查
    const std::string test_message = "A message that should be buffered.";
    LOG_INFO(test_message.c_str());

    // 立刻读取日志文件，此时文件甚至可能还未创建，或者为空
    // 因为刷新间隔(2s)还远远没有到
    auto lines_before_flush = read_all_lines_from_dir(test_log_dir);
    ASSERT_TRUE(lines_before_flush.empty()) << "Log should be buffered and not written to file immediately.";

    // 3. Act & Assert Phase 2: 等待超过刷新间隔后检查
    // 等待3秒，确保超过了2秒的刷新周期
    std::this_thread::sleep_for(std::chrono::seconds(3));

    auto lines_after_flush = read_all_lines_from_dir(test_log_dir);
    ASSERT_EQ(lines_after_flush.size(), 1) << "Log should have been flushed to file after the interval.";
    ASSERT_NE(lines_after_flush[0].find(test_message), std::string::npos) << "The flushed log content is incorrect.";
}

/**
 * @brief 测试边界情况：当刷新间隔设置为0时，应立即刷新
 */
TEST_F(LoggerTest, NoBufferingWhenFlushIntervalIsZero) {
    // 1. Arrange: 配置刷新间隔为0
    Logger::Config config;
    config.log_folder = test_log_dir.string();
    config.max_queue_size = 1024; // 异步模式
    config.flush_interval_seconds = 0; // 关键配置：立即刷新
    config.enable_console_sink = true;
    config.is_override = true;

    Logger::get_instance().init(config);

    // 2. Act
    const std::string test_message = "This should be flushed immediately.";
    LOG_INFO(test_message.c_str());

    // 3. Assert
    // 稍微等待一下，给后台线程一个执行周期的时间
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto lines = read_all_lines_from_dir(test_log_dir);
    ASSERT_EQ(lines.size(), 1) << "Log should be written immediately when flush interval is 0.";
    ASSERT_NE(lines[0].find(test_message), std::string::npos) << "The flushed log content is incorrect.";
}

/**
 * @brief 测试鲁棒性：在程序退出(stop)时，应强制刷新所有缓冲的日志
 */
TEST_F(LoggerTest, FinalFlushOnStop) {
    // 1. Arrange: 配置一个很长的刷新间隔，确保在测试期间不会自动刷新
    Logger::Config config;
    config.log_folder = test_log_dir.string();
    config.max_queue_size = 1024;
    config.flush_interval_seconds = 60; // 一个足够长的时间
    config.enable_console_sink = true;
    config.is_override = true;

    Logger::get_instance().init(config);

    // 2. Act
    const std::string test_message = "A final message before logger stops.";
    LOG_ERROR(test_message.c_str());

    // 立即检查，确认日志仍在缓冲区中
    auto lines_before_stop = read_all_lines_from_dir(test_log_dir);
    ASSERT_TRUE(lines_before_stop.empty()) << "Log should still be in buffer before stop() is called.";

    // 手动调用 stop()，这将触发最终的刷新
    Logger::get_instance().stop();

    // 3. Assert
    auto lines_after_stop = read_all_lines_from_dir(test_log_dir);
    ASSERT_EQ(lines_after_stop.size(), 1) << "All buffered logs should be flushed on stop().";
    ASSERT_NE(lines_after_stop[0].find(test_message), std::string::npos) << "The final flushed log content is incorrect.";
}


// ===================================================================
// 启用周期刷新后的压力测试
// ===================================================================

TEST_F(LoggerTest, AsyncMode_MultipleThreads_NoDataLoss_Enable_Flush_interval) {
    Logger::Config config;
    config.log_folder = test_log_dir.string();
    config.max_queue_size = 100000;
    config.max_lines_per_file = 5000; // 设置一个较小的行数，强制在测试中发生文件切分
    config.flush_interval_seconds = 1;
    config.enable_console_sink = true;
    config.is_override = true;

    Logger& log = Logger::get_instance();
    log.init(config);

    std::string heavy_string(100, 'd');

    // const int num_threads = 10;
    // const int messages_per_thread = 1000;
    // const int total_messages = num_threads * messages_per_thread;
    //
    // std::vector<std::thread> threads;
    // for (int i = 0; i < num_threads; ++i) {
    //     threads.emplace_back([&log, i, messages_per_thread]() {
    //         for (int j = 0; j < messages_per_thread; ++j) {
    //             log.info("Thread {} writing message {}", i, heavy_string);
    //         }
    //     });
    // }
    //
    // for (auto& t : threads) {
    //     t.join();
    // }

    int total_messages = 100000;

    // 使用单线程直观判断
    for(int i = 0; i < 100000; ++i) {
        LOG_INFO("Thread {} writing message {}", i, heavy_string);
    }

    std::this_thread::sleep_for(std::chrono::seconds(10));
    // 停止logger，确保所有日志都写入磁盘
    Logger::get_instance().stop();

    // *** 修改点: 验证逻辑不变，但现在更强大，因为它会遍历所有切分后的文件 ***
    auto all_lines_read = read_all_lines_from_dir(test_log_dir);
    ASSERT_EQ(all_lines_read.size(), total_messages);

    // 可选的额外验证：确保文件数量是正确的
    // total_messages / max_lines_per_file + (total_messages % max_lines_per_file != 0)
    size_t expected_file_count = (total_messages + config.max_lines_per_file - 1) / config.max_lines_per_file;
    ASSERT_EQ(count_files_in_dir(test_log_dir), expected_file_count);
}