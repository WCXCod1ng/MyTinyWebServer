//
// Created by user on 2025/11/11.
//
#include "../../lib/thread_pool/thread_pool.h"

#include "gtest/gtest.h"

#include <chrono>
#include <thread>
#include <vector>
#include <numeric>
#include <atomic>
#include <stdexcept>
#include <random>

// 使用 using namespace std::chrono_literals; 可以让你使用 100ms 这样的时间单位
using namespace std::chrono_literals;


class ThreadPoolTest : public ::testing::Test {
protected:
    // SetUp() 会在每个 TEST_F 测试用例运行前被调用
    void SetUp() override {
        // 为大多数测试创建一个有4个线程的池
        pool = std::make_unique<ThreadPool>(4);
    }

    // TearDown() 会在每个 TEST_F 测试用例运行后被调用
    void TearDown() override {
        // unique_ptr 会自动销毁 ThreadPool 对象，调用其析构函数
    }

    // 将 pool 定义为智能指针，因为它没有默认构造函数
    std::unique_ptr<ThreadPool> pool;
};

int multiply(int a, int b) {
    return a * b;
}

/// 基本任务提交流程与返回值获取
TEST_F(ThreadPoolTest, BasicSubmissionAndReturnValue) {
    auto future_result = pool->enqueue(multiply, 5, 10);
    int result = future_result.get();

    // 使用 GTest 的断言宏，比 assert() 提供了更丰富的输出信息
    EXPECT_EQ(result, 50);
}

/// 无返回值任务 (void) 与 Lambda 表达式
TEST_F(ThreadPoolTest, VoidReturnAndLambda) {
    std::atomic<int> counter{0}; // 使用局部原子变量，避免测试间的状态污染

    auto f1 = pool->enqueue([&counter]() {
        ++counter;
    });

    auto f2 = pool->enqueue([&counter]() {
        counter += 2;
    });

    // 等待任务完成
    f1.get();
    f2.get();

    EXPECT_EQ(counter.load(), 3);
}


void throw_exception_task() {
    throw std::runtime_error("This is a test exception.");
}

/// 异常处理
TEST_F(ThreadPoolTest, ExceptionHandling) {
    auto future_exception = pool->enqueue(throw_exception_task);

    // GTest 提供了专门用于测试异常的宏，非常简洁
    EXPECT_THROW(future_exception.get(), std::runtime_error);
}


/// 析构函数的优雅停机 (Graceful Shutdown)
/// 使用 TEST 宏，因为它不依赖于固件
TEST(ThreadPoolShutdownTest, GracefulShutdown) {
    std::atomic<int> counter{0};

    { // 使用花括号创建一个作用域来控制 pool 的生命周期
        ThreadPool pool(2);
        for (int i = 0; i < 10; ++i) {
            pool.enqueue([&counter]() {
                std::this_thread::sleep_for(20ms); // 模拟耗时任务
                ++counter;
            });
        }
        // 当离开这个作用域时，pool 的析构函数会被自动调用
    } // <-- 析构函数在这里被调用，它应该阻塞直到所有任务完成

    // 验证所有任务是否都已执行
    EXPECT_EQ(counter.load(), 10);
}

/// 压力测试
TEST_F(ThreadPoolTest, StressTestHighConcurrency) {
    constexpr int num_tasks = 10000;
    std::atomic<long long> total_sum{0};

    std::vector<std::future<void>> futures;
    futures.reserve(num_tasks);

    for (int i = 0; i < num_tasks; ++i) {
        futures.emplace_back(pool->enqueue([&total_sum, i]() {
            static thread_local std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(20, 50);

            int ms = dist(rng);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            total_sum += i;
        }));
    }

    // 等待所有任务完成
    for (auto& f : futures) {
        f.get();
    }

    // 验证结果的正确性
    long long expected_sum = 0;
    for (int i = 0; i < num_tasks; ++i) {
        expected_sum += i;
    }

    EXPECT_EQ(total_sum.load(), expected_sum);
}