//
// Created by user on 2025/11/12.
//

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H


#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <optional>


/// 定义一个阻塞队列，用以实现异步任务
template <typename T>
class BlockingQueue {
public:
    // 构造函数，可以指定队列的最大容量。0表示无限制。
    explicit BlockingQueue(size_t max_size = 0)
        : m_max_size(max_size), m_is_closed(false) {}

    ~BlockingQueue() {
        this->close();
    }

    /// 禁止拷贝和赋值
    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    /// 向队列中放入一个元素。如果队列已满，则会阻塞等待。
    /// 使用右值引用和std::forward实现完美转发，避免不必要的拷贝。
    /// 返回是否push成功，因为如果队列关闭就会push失败。发生在push和队列关闭并发执行时
    template<typename U>
    bool push(U&& item);

    /// 从队列中取出一个元素。如果队列为空，则会阻塞等待。
    /// 返回一个有效值表示成功取出，返回空表示队列已关闭且为空。
    std::optional<T> pop();

    /// 带超时时间的pop
    /// 和pop的区别在于，返回时可能的原因还包括：等待时间到期（pop的原因只有：队列关闭且为空）
    template<typename Rep, typename Period>
    std::optional<T> pop_for(const std::chrono::duration<Rep, Period>& timeout);

    /// 关闭队列。所有等待的线程将被唤醒，并且后续的push操作将失效。
    void close();

    /// 检查队列是否已关闭。
    bool is_closed() const;

    /// 获取当前队列中的元素数量。
    size_t size() const;

protected:
    // 底层使用标准库的queue，它默认由deque实现，效率很高。
    std::queue<T> m_queue;
    mutable std::mutex m_mutex; // 需要是mutable，以便在const成员函数中加锁

    // 使用两个条件变量，分别用于生产者和消费者，可以减少不必要的唤醒。
    std::condition_variable m_cond_producer;
    std::condition_variable m_cond_consumer;

    // 最大容量，0：阻塞队列容量无限制；正整数：指定阻塞队列的上限
    size_t m_max_size;
    std::atomic<bool> m_is_closed; // 使用原子变量确保关闭状态的线程安全
};


template<typename T>
template<typename U>
bool BlockingQueue<T>::push(U &&item) {
    // 上锁
    std::unique_lock<std::mutex> lock(m_mutex);
    // 如果有容量上限，则需要阻塞等待
    if(m_max_size > 0) {
        // 阻塞等待容量小于上界，而且能够避免虚假唤醒
        m_cond_producer.wait(lock, [this]() {
           return m_is_closed || m_queue.size() < m_max_size;
        });
    }

    // 到此依然持有锁
    // 如果队列被关闭，现在不是直接抛异常，而是返回false，让调用者处理
    if(m_is_closed) {
        return false;
        // throw std::runtime_error("Unable to push an element into a closed blocking queue");
    }

    // 向队列中插入一个元素
    // 这里可以使用emplace（因为push总是会执行一次拷贝），注意forward中的模板参数是item的类型U
    m_queue.emplace(std::forward<U>(item));

    lock.unlock(); // 提前解锁，性能优化

    // 唤醒消费者
    m_cond_consumer.notify_one();

    // 正常则返回true
    return true;
}


template<typename T>
std::optional<T> BlockingQueue<T>::pop() {
    // 上锁
    std::unique_lock<std::mutex> lock(m_mutex);

    // 阻塞等待直到队列不为空，或者队列被关闭
    m_cond_consumer.wait(lock, [this]() {
        return m_is_closed || !m_queue.empty();
    });

    // 如果队列被关闭，则依然能处理，直到队列也为空
    if(m_is_closed && m_queue.empty()) {
        return std::nullopt;
    }

    // 从队列取出一个元素
    T item = std::move(m_queue.front());
    m_queue.pop();

    // 提前解锁，性能优化
    lock.unlock();

    // 唤醒生产者
    m_cond_producer.notify_one();

    return item; // 直接返回，会自动构造
}

template<typename T>
template <typename Rep, typename Period>
std::optional<T> BlockingQueue<T>::pop_for(const std::chrono::duration<Rep, Period>& timeout) {
    std::unique_lock<std::mutex> lock(m_mutex);

    // wait_for会阻塞直到被唤醒且谓词为true，或者超时。
    // 如果因谓词满足而返回，函数返回true；如果因超时而返回，函数返回false。
    bool success = m_cond_consumer.wait_for(lock, timeout, [this]() {
        return m_is_closed || !m_queue.empty();
    });

    // 如果等待失败（超时），直接返回
    if (!success) {
        return std::nullopt; // 超时
    }

    // 如果等待成功，但队列是因关闭而唤醒且队列已空
    if (m_is_closed && m_queue.empty()) {
        return std::nullopt; // 队列关闭
    }

    // 等待成功，且队列中有元素
    T item = std::move(m_queue.front());
    m_queue.pop();

    lock.unlock(); // 解锁
    m_cond_producer.notify_one(); // 唤醒生产者

    return item;
}


template<typename T>
void BlockingQueue<T>::close() {
    {
        // 关闭阻塞队列时也需要加锁
        std::lock_guard<std::mutex> lock(m_mutex);
        if(m_is_closed) {
            return;
        }

        // 标记flag
        m_is_closed.store(true);
    } // 到此自动解锁

    // 唤醒所有阻塞的线程
    m_cond_producer.notify_all();
    m_cond_consumer.notify_all();
}


template<typename T>
bool BlockingQueue<T>::is_closed() const {
    return m_is_closed.load();
}


template<typename T>
size_t BlockingQueue<T>::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
} // 到此会自动解锁




#endif //BLOCK_QUEUE_H
