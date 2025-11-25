//
// Created by user on 2025/11/13.
//

#ifndef NONBLOCK_QUEUE_H
#define NONBLOCK_QUEUE_H
#include "block_queue.h"


template<typename T>
class ConcurrentQueue: public BlockingQueue<T>{
public:
    using BlockingQueue<T>::BlockingQueue;

    /// 增加try_push，如果队列已满或已关闭，则会立即返回false
    /// 如果队列已满或者已关闭，则会立即返回false
    /// 如果放入成功则会返回true
    template<typename U>
    bool try_push(U&& item) {
        std::unique_lock<std::mutex> lock(this->m_mutex);

        // 检查是否已经关闭
        if(this->m_is_closed) {
            return false;
        }

        // 检查是否已满，如果已满则直接退出（这是非阻塞队列的核心）
        if(this->m_max_size > 0 && this->m_queue.size() >= this->m_max_size) {
            return false;
        }

        // 到此可以实际插入元素
        this->m_queue.emplace(std::forward<U>(item));

        lock.unlock(); // 解锁

        this->m_cond_consumer.notify_one(); // 仍然需要唤醒可能在等待的消费者（因为有其他线程可能是调用阻塞版的pop）

        return true;
    }

    /// 增加try_pop，尝试从队列中取出一个元素
    /// 如果队列为空，则会立即返回std::nullopt
    /// 成功取出则返回包含元素的optional
    std::optional<T> try_pop() {
        std::unique_lock<std::mutex> lock(this->m_mutex);

        // 检查队列是否为空
        if(this->m_queue.empty()) {
            return std::nullopt;
        }

        // 到此说明队列中一定有元素（不用关心是否关闭）
        T item = std::move(this->m_queue.front());
        this->m_queue.pop();

        lock.unlock(); // 解锁

        this->m_cond_producer.notify_one(); // 仍然需要唤醒可能在等待的生产者（因为还有其他线程可能是调用阻塞版的push）

        return item;
    }
};



#endif //NONBLOCK_QUEUE_H
