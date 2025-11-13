//
// Created by user on 2025/11/12.
//

#include "gtest/gtest.h"
#include "../../lib/utils/block_queue.h" // 确保这个头文件在你的包含路径中
#include <thread>
#include <vector>
#include <numeric>
#include <chrono>
#include <future>

// 使用TEST_F创建测试夹具，方便后续扩展
class BlockingQueueTest : public ::testing::Test {};

// ====================================================================================
// 功能正确性与边界条件 (Correctness and Corner Cases)
// ====================================================================================

TEST_F(BlockingQueueTest, BasicPushPop) {
    BlockingQueue<int> queue(5);
    ASSERT_EQ(queue.size(), 0);

    queue.push(42);
    ASSERT_EQ(queue.size(), 1);

    auto item_opt = queue.pop();
    ASSERT_TRUE(item_opt.has_value());
    EXPECT_EQ(item_opt.value(), 42);
    ASSERT_EQ(queue.size(), 0);
}

TEST_F(BlockingQueueTest, FIFOOrder) {
    BlockingQueue<int> queue(3);
    queue.push(1);
    queue.push(2);
    queue.push(3);

    auto item1 = queue.pop();
    auto item2 = queue.pop();
    auto item3 = queue.pop();

    ASSERT_TRUE(item1.has_value() && item2.has_value() && item3.has_value());
    EXPECT_EQ(item1.value(), 1);
    EXPECT_EQ(item2.value(), 2);
    EXPECT_EQ(item3.value(), 3);
}

TEST_F(BlockingQueueTest, BlocksOnEmptyThenPops) {
    BlockingQueue<int> queue(1);

    std::promise<int> popped_value_promise;
    std::future<int> popped_value_future = popped_value_promise.get_future();

    std::thread consumer([&]() {
        auto item_opt = queue.pop();
        if (item_opt) {
            popped_value_promise.set_value(item_opt.value());
        }
    });

    // 等待一小段时间，确保消费者线程已经启动并阻塞在pop()上
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    queue.push(99);

    // 等待消费者线程的结果，设置超时以防测试挂起
    auto status = popped_value_future.wait_for(std::chrono::seconds(1));
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(popped_value_future.get(), 99);

    consumer.join();
}

TEST_F(BlockingQueueTest, BlocksOnFullThenPushes) {
    BlockingQueue<int> queue(1);
    queue.push(10); // 队列已满

    std::atomic<bool> push_completed = false;

    std::thread producer([&]() {
        queue.push(20);
        push_completed = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // 此时，生产者线程应阻塞，push未完成
    ASSERT_FALSE(push_completed);

    auto item_opt = queue.pop(); // 腾出空间
    ASSERT_TRUE(item_opt.has_value());
    EXPECT_EQ(item_opt.value(), 10);

    // 等待生产者完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(push_completed);

    EXPECT_EQ(queue.size(), 1);

    producer.join();
}

TEST_F(BlockingQueueTest, CloseUnblocksEmptyPop) {
    BlockingQueue<int> queue;

    std::promise<bool> pop_result_promise;
    std::future<bool> pop_result_future = pop_result_promise.get_future();

    std::thread consumer([&]() {
        auto item_opt = queue.pop();
        pop_result_promise.set_value(item_opt.has_value());
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    queue.close();

    auto status = pop_result_future.wait_for(std::chrono::seconds(1));
    ASSERT_EQ(status, std::future_status::ready);
    // pop应该返回一个没有值的optional
    EXPECT_FALSE(pop_result_future.get());

    consumer.join();
}

TEST_F(BlockingQueueTest, PopAllItemsAfterClose) {
    BlockingQueue<int> queue(5);
    queue.push(1);
    queue.push(2);

    queue.close();
    ASSERT_TRUE(queue.is_closed());

    // 即使队列关闭，也应该能取出所有剩余的元素
    auto item1 = queue.pop();
    ASSERT_TRUE(item1.has_value());
    EXPECT_EQ(item1.value(), 1);

    auto item2 = queue.pop();
    ASSERT_TRUE(item2.has_value());
    EXPECT_EQ(item2.value(), 2);

    // 队列为空后，再次pop应该返回nullopt
    auto empty_item = queue.pop();
    ASSERT_FALSE(empty_item.has_value());
}

TEST_F(BlockingQueueTest, PushToClosedQueueDoesNothing) {
    BlockingQueue<int> queue;
    queue.close();
    ASSERT_EQ(queue.push(100), false); // 应该返回false，表示插入失败

    ASSERT_EQ(queue.size(), 0); // 并且队列为空
}


// ====================================================================================
// 压力与并发测试 (Stress and Concurrency Tests)
// ====================================================================================

TEST_F(BlockingQueueTest, MultipleProducersSingleConsumer) {
    BlockingQueue<int> queue(100);
    const int num_producers = 4;
    const int items_per_producer = 1000;
    const int total_items = num_producers * items_per_producer;

    std::vector<std::thread> producers;
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&, i]() {
            for (int j = 0; j < items_per_producer; ++j) {
                // 每个生产者产生不同的数字范围以避免混淆
                queue.push(i * items_per_producer + j);
            }
        });
    }

    std::vector<int> consumed_items;
    consumed_items.reserve(total_items);
    int items_consumed = 0;
    while (items_consumed < total_items) {
        if (auto item_opt = queue.pop()) {
            consumed_items.push_back(item_opt.value());
            items_consumed++;
        } else {
            // 不应该在消费完所有物品前关闭
            FAIL() << "Queue closed prematurely.";
        }
    }

    for (auto& t : producers) {
        t.join();
    }

    ASSERT_EQ(consumed_items.size(), total_items);
}

TEST_F(BlockingQueueTest, MultipleProducersMultipleConsumers) {
    BlockingQueue<long long> queue(100);
    const int num_producers = 8;
    const int num_consumers = 8;
    const int items_per_producer = 10000;
    const long long total_items = num_producers * items_per_producer;

    std::atomic<long long> total_sum_produced = 0;
    std::atomic<long long> total_sum_consumed = 0;
    std::atomic<long long> items_produced_count = 0;

    std::vector<std::thread> producers;
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&]() {
            long long local_sum = 0;
            for (int j = 0; j < items_per_producer; ++j) {
                long long val = items_produced_count.fetch_add(1);
                local_sum += val;
                queue.push(val);
            }
            total_sum_produced += local_sum;
        });
    }

    std::atomic<long long> items_consumed_count = 0;
    std::vector<std::thread> consumers;
    for (int i = 0; i < num_consumers; ++i) {
        consumers.emplace_back([&]() {
            long long local_sum = 0;
            while(true) {
                auto item_opt = queue.pop();
                if (item_opt) {
                    local_sum += item_opt.value();
                    items_consumed_count++;
                } else {
                    // 队列已关闭且为空，消费者退出
                    break;
                }
            }
            total_sum_consumed += local_sum;
        });
    }

    // 等待所有生产者完成
    for (auto& t : producers) {
        t.join();
    }

    // 所有产品都生产完毕，关闭队列以通知消费者
    queue.close();

    // 等待所有消费者完成
    for (auto& t : consumers) {
        t.join();
    }

    ASSERT_EQ(items_produced_count, total_items);
    ASSERT_EQ(items_consumed_count, total_items);
    ASSERT_EQ(total_sum_produced, total_sum_consumed);
}


// ====================================================================================
// 专门针对于pop_for的测试
// ====================================================================================


class PopForTest : public ::testing::Test {
protected:
    BlockingQueue<int> q;
};


// 测试场景1: 当队列持续为空时，pop_for应该超时并返回nullopt
TEST_F(PopForTest, TimesOutWhenQueueIsEmpty) {
    using namespace std::chrono;

    const auto timeout = milliseconds(50);
    auto start = steady_clock::now();

    // 在空队列上调用pop_for
    auto result = q.pop_for(timeout);

    auto end = steady_clock::now();
    auto elapsed = duration_cast<milliseconds>(end - start);

    // 断言1: 结果应该是空的 (超时)
    ASSERT_FALSE(result.has_value());

    // 断言2: 等待的时间应该约等于超时时间
    // 我们检查它是否大于等于超时时间，因为线程调度可能会有微小延迟
    ASSERT_GE(elapsed, timeout);
    // 也可以检查一个合理的上限，确保它没有等待太久
    ASSERT_LT(elapsed, timeout * 2);
}

// 测试场景2: 当队列非空时，pop_for应该立即返回元素，不等待
TEST_F(PopForTest, ReturnsImmediatelyWhenQueueIsNotEmpty) {
    using namespace std::chrono;

    q.push(42);

    auto start = steady_clock::now();
    // 使用一个很长的超时时间，以证明函数不会等待
    auto result = q.pop_for(seconds(5));
    auto end = steady_clock::now();
    auto elapsed = duration_cast<milliseconds>(end - start);

    // 断言1: 结果应该包含一个值
    ASSERT_TRUE(result.has_value());
    // 断言2: 值应该是正确的
    ASSERT_EQ(*result, 42);
    // 断言3: 操作应该非常快，远小于超时时间
    ASSERT_LT(elapsed, milliseconds(10));
}

// 测试场景3: 当pop_for等待时，有元素被推入，应成功返回该元素
TEST_F(PopForTest, SucceedsWhenItemPushedBeforeTimeout) {
    using namespace std::chrono;

    // 启动一个生产者线程，它将在短暂延迟后推入一个元素
    std::thread producer([this]() {
        std::this_thread::sleep_for(milliseconds(50));
        q.push(99);
    });

    const auto timeout = seconds(1);
    auto start = steady_clock::now();

    auto result = q.pop_for(timeout);

    auto end = steady_clock::now();
    auto elapsed = duration_cast<milliseconds>(end - start);

    producer.join();

    // 断言1: 结果应该包含一个值
    ASSERT_TRUE(result.has_value());
    // 断言2: 值应该是生产者推入的值
    ASSERT_EQ(*result, 99);
    // 断言3: 等待时间应大于生产者的睡眠时间，但远小于总超时
    ASSERT_GE(elapsed, milliseconds(50));
    ASSERT_LT(elapsed, timeout);
}

// 测试场景4: 当pop_for等待时，队列被关闭，应立即返回nullopt
TEST_F(PopForTest, WakesUpImmediatelyWhenQueueIsClosed) {
    using namespace std::chrono;

    // 启动一个线程，在短暂延迟后关闭队列
    std::thread closer([this]() {
        std::this_thread::sleep_for(milliseconds(50));
        q.close();
    });

    const auto timeout = seconds(5); // 使用一个很长的超时
    auto start = steady_clock::now();

    auto result = q.pop_for(timeout);

    auto end = steady_clock::now();
    auto elapsed = duration_cast<milliseconds>(end - start);

    closer.join();

    // 断言1: 结果应该是空的，因为队列被关闭了
    ASSERT_FALSE(result.has_value());
    // 断言2: 等待时间应约等于关闭线程的睡眠时间，证明是close唤醒了它，而不是超时
    ASSERT_GE(elapsed, milliseconds(50));
    ASSERT_LT(elapsed, timeout);
}

// 测试场景5: 使用零超时，其行为应类似于try_pop
TEST_F(PopForTest, BehavesLikeTryPopWithZeroTimeout) {
    // 情况A: 队列为空
    auto result1 = q.pop_for(std::chrono::seconds(0));
    ASSERT_FALSE(result1.has_value());

    // 情况B: 队列非空
    q.push(101);
    auto result2 = q.pop_for(std::chrono::seconds(0));
    ASSERT_TRUE(result2.has_value());
    ASSERT_EQ(*result2, 101);
}