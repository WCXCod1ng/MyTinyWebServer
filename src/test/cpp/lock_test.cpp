//
// Created by user on 2025/11/10.
//
#include <format>
#include <queue>
#include <gtest/gtest.h>

// 测试套件名称: LockTest
// 测试用例名称: SemaphoreTest，我们使用它可以直接替代从pthread库中使用的sem
TEST(LockTest, SemaphoreTest) {
    // 1. 设定场景：
    // 我们创建一个最多只允许3个并发访问的信号量。
    constexpr int MAX_CONCURRENT_THREADS = 3;
    std::counting_semaphore<> semaphore(MAX_CONCURRENT_THREADS);

    // 我们将创建10个线程去争抢这3个“名额”。
    constexpr int TOTAL_THREADS = 10;

    // 用于记录当前同时在“临界区”内的线程数量。
    // 使用 atomic 来保证多线程下的读写安全。
    std::atomic<int> concurrent_threads_counter {0};

    // 用于记录在测试期间，我们观察到的最大并发数。
    std::atomic<int> max_observed_concurrency {0};

    // 2. 创建并启动线程
    std::vector<std::thread> threads;
    for (int i = 0; i < TOTAL_THREADS; ++i) {
        threads.emplace_back([&]() {
            // a. 请求资源 (P操作)
            // 线程会在这里等待，直到信号量计数 > 0
            semaphore.acquire();

            // --- 进入临界区 ---

            // b. 记录当前并发数，并更新最大并发数记录
            int current_count = ++concurrent_threads_counter;

            // 为了捕获峰值，我们用一个循环来更新最大值
            int observed_max = max_observed_concurrency.load();
            while(current_count > observed_max) {
                max_observed_concurrency.compare_exchange_weak(observed_max, current_count);
            }

            // c. 核心断言：我们期望在任何时刻，
            //    临界区内的线程数都不能超过信号量的初始值。
            EXPECT_LE(current_count, MAX_CONCURRENT_THREADS);

            // 模拟一些工作
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            --concurrent_threads_counter;
            // --- 离开临界区 ---

            // d. 释放资源 (V操作)
            // 信号量计数加一，可能会唤醒一个等待的线程
            semaphore.release();
        });
    }

    // 3. 等待所有线程执行完毕
    for (auto& t : threads) {
        t.join();
    }

    // 4. 最终验证
    // 检查最终观察到的最大并发数是否确实符合预期。
    // 这是一个总览性的断言，提供了额外的信心。
    std::cout << "Max observed concurrency was: " << max_observed_concurrency << std::endl;
    EXPECT_EQ(max_observed_concurrency, MAX_CONCURRENT_THREADS);
}



// 测试标准库提供的Mutex，用于替代pthread提供的mutex
TEST(LockTest, MutexTest) {
    long long g_counter = 0; // 需要并发修改的计数器
    std::mutex g_mutex; // 创建一个全局的互斥锁实例
    auto increment_safe = [&]() {
        for(int i = 0; i < 1000000; ++i) {
            // // 手动加锁和解锁，不推荐使用，容易忘记解锁而导致死锁
            // g_mutex.lock();
            // g_counter++;
            // g_mutex.unlock();

            // 通过g_mutex封装得到lock_guard，它的好处是可以在构造函数中**自动加锁**，在析构函数中**自动解锁**
            std::lock_guard<std::mutex> lg(g_mutex);
            g_counter++;
        } // 当lg在这里离开作用域时，其析构函数就会自动调用g_mutex.unlock()
    };

    // 创建两个线程
    std::thread t1(increment_safe);
    std::thread t2(increment_safe);

    t1.join();
    t2.join();

    std::cout << std::format("Final counter value: {}", g_counter) << std::endl;

    EXPECT_EQ(2000000, g_counter);
}


//  测试标准库提供的CondVar，用于替代pthread提供的cond
TEST(LockTest, CondTest) {
    std::queue<int> data_queue; // 共享数据队列
    std::condition_variable cv;
    std::mutex mtx;
    const unsigned int MAX_QUEUE_SIZE = 5;

    std::vector<int> produced_data;
    std::vector<int> consumed_data;

    // 生产者线程
    auto producer = [&](int id) -> void {
        for(int i = 0; i < 10; ++i) {
            // 模拟生产一个产品
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // 1. 构造一个unique_lock锁住mutex
            // unique_lock在构造时会自动加锁，析构时自动解锁；并且它比lock_guard更灵活，可以手动解锁和重新加锁，通常用于条件变量
            std::unique_lock<std::mutex> lock(mtx);


            // 2. 等待条件变量，直到队列有空间
            // 如下是wait的第二种用法，传入一个lambda表达式作为条件，它本质上是一个while循环
            // while(data_queue.size() >= MAX_QUEUE_SIZE) { cv.wait(lock); }
            // 而wait(lock)会：1.解锁mutex 2.阻塞等待 3.被唤醒后重新加锁mutex
            cv.wait(lock, [&] {return data_queue.size() < MAX_QUEUE_SIZE; });
            // 如果要等待一定的时间，可以使用wait_for或wait_until
            // cv.wait_for(lock, std::chrono::milliseconds(100), [&] {return data_queue.size() < MAX_QUEUE_SIZE; }); // 等待至多100毫秒
            // cv.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(100), [&] {return data_queue.size() < MAX_QUEUE_SIZE; }); // 等待至多100毫秒

            // 3. 到此说明条件满足（队列有空间），可以生产数据了。并且mutex已经重新加锁
            int data = id * 100 + i;
            data_queue.push(data);
            std::cout << std::format("Producer {} produced data: {}, queue size: {}\n", id, data, data_queue.size());
            produced_data.push_back(data);

            // 4. 生产完毕，通知消费者线程可以消费了
            cv.notify_one(); // 这里通知一个等待的消费者，如果要通知所有消费者可以用cv.notify_all();

            // lock在这里离开作用域，自动解锁
        }
    };

    // 消费者线程
    auto consumer = [&](int id) -> void {
        for(int i = 0; i < 10; ++i) {
            // 模拟消费一个产品
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // 1. 构造一个unique_lock锁住mutex
            std::unique_lock<std::mutex> lock(mtx);

            // 2. 等待条件变量，直到队列非空
            cv.wait(lock, [&] {return !data_queue.empty(); });

            // 3. 到此说明条件满足（队列非空），可以消费数据了。并且mutex已经重新加锁
            int data = data_queue.front();
            data_queue.pop();
            std::cout << std::format("Consumer {} consumed data: {}, queue size: {}\n", id, data, data_queue.size());
            consumed_data.push_back(data);

            // 4. 消费完毕，通知生产者线程可以继续生产了
            cv.notify_one(); // 这里通知一个等待的生产者，如果要通知所有生产者可以用cv.notify_all();

            // lock在这里离开作用域，自动解锁
        }
    };

    std::thread p1(producer, 1);
    std::thread c1(consumer, 1);

    p1.join();
    c1.join();


    // 5. 最终验证，由于生产者只有一个，消费者也只有一个，生产的数据和消费的数据应该是一样的（包括数量和内容）
    EXPECT_TRUE(produced_data == consumed_data);
}