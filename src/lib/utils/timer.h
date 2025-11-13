//
// Created by user on 2025/11/11.
//

#ifndef TIMER_H
#define TIMER_H



// --- Timer.h (模拟) ---
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <vector>

// 简单的定时器管理器
class TimerManager {
public:
    using Clock = std::chrono::steady_clock;
    using Timestamp = Clock::time_point;
    using Duration = Clock::duration;
    using TimerCallback = std::function<void()>;
    // 使用已连接socket的fd作为定时器的唯一ID
    using TimerId = int;

    TimerManager() = default;

    ~TimerManager() = default;

    // --- 核心公共接口 ---

    /// 添加一个定时器
    /// 它在指定的超时时间后执行回调函数。
    void add_timer(TimerId id, Duration timeout, TimerCallback cb);

    /// 调整（刷新）一个已存在定时器的超时时间
    /// 当一个不活跃的连接重新收到数据时，就需要调用此函数。
    void adjust_timer(TimerId id, Duration new_timeout);

    /// 移除一个定时器
    /// 当一个连接被正常关闭时，用此函数移除其关联的定时器
    void remove_timer(TimerId id);

    /// 检查并处理所有到期的定时器。此函数应在主事件循环中被周期性调用。
    /// 它遍历整个哈希表，将所有已经到期（expires <= Clock::now()）的定时器找出来。然后，它安全地执行这些到期定时器的回调函数，并从哈希表中将它们删除。这个“两步走”（先收集ID，再处理）的策略确保了在执行回调时，即使回调函数本身也修改了m_timers（例如，添加一个新的定时器），也不会导致迭代器失效等问题。
    void tick();

private:
    // --- 内部数据结构 ---

    // 定时器节点，存储到期时间和回调
    struct TimerNode {
        Timestamp expires; // 存储该定时器精确到期的时间点
        TimerCallback callback; // 存储一个可调用对象，即定时器到期时需要执行的操作
    };

    // 使用哈希表存储所有定时器，通过ID可以实现 O(1) 的平均查找、添加和删除效率
    // note 但是在tick的时候是需要遍历所有的定时任务的，因为没有按照超时时间排序，所以需要遍历（而原版则是维护一个有序的链表）
    // fixme 性能优化点，使用更复杂但是更高效的数据结构
    std::unordered_map<TimerId, TimerNode> m_timers;
};



#endif //TIMER_H
