//
// Created by user on 2025/11/27.
//

#ifndef TIMERQUEUE_H
#define TIMERQUEUE_H

#include <set>
#include <vector>

#include "../base/NonCopyable.h"
#include "../base/TimeStamp.h"
#include "Callbacks.h"
#include "Channel.h"

class EventLoop;
class Timer;
class TimerId;

/// 定时器队列
/// 管理所有的Timer，并拥有一个timerfd和它对应的Channel
class TimerQueue : NonCopyable {
public:
    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    /// 用户接口：插入定时器 (线程安全，会跨线程调用)
    /// @param cb 定时器触发（到期）后应该执行的回调函数
    /// @param expiration 下一次过期的时间戳
    /// @param interval 定时器重复的间隔，设置为0表示只触发1次
    /// @return 新加入的定时器的句柄
    TimerId addTimer(TimerCallback cb, TimeStamp expiration, double interval);

    /// 用户接口：取消定时器
    /// @param timerId 需要取消的定时器所对应的句柄
    void cancel(TimerId timerId);

private:
    // TimerQueue 属于 Loop 线程，以下函数只能在 Loop 线程运行
    using Entry = std::pair<TimeStamp, Timer*>;
    using TimerList = std::set<Entry>;
    using ActiveTimer = std::pair<Timer*, int64_t>;
    using ActiveTimerSet = std::set<ActiveTimer>;

    /// timerfd 的 Channel 读回调，当定时器到达后会timerfd可读，此时就会调用该函数
    /// 叫handleTimerExpirationEvent更为合理，表明它是定时器到期的事件处理器
    void handleChannelRead();

    /// 从定时器队列中获取并移除所有已过期的定时器
    std::vector<Entry> getExpired(TimeStamp now);

    /// 重置给定的定时器（如果是重复的则再次添加，是一次性的则 delete）
    /// @param expired 过期的定时器
    /// @param now 当前的时间，用来计算下一次过期的时间
    void reset(const std::vector<Entry>& expired, TimeStamp now);

    /// 插入定时器的内部实现
    /// @param timer 待插入的定时器指针
    /// @return 插入后，TimerQueue中最早到期的定时器是否发生了变化
    bool insert(Timer* timer);

    /// 所属的事件循环
    EventLoop* loop_;
    /// 对应内核给它分配的timerId
    const int timerfd_;
    /// 该timerId对应的Channel（将来定时器定时事件会被转化为IO事件，由Reactor（EventLoop+Epoll）统一监控
    Channel timerfdChannel_;

    // 核心数据结构：按时间排序的定时器列表，注意使用 std::set 来存储 Timer。
    // Key: std::pair<TimeStamp, Timer*>。
    // 好处：set 会自动按照 TimeStamp 排序。我们只需要看 set.begin() 就能拿到最早要过期的那个任务。同时由于加入了 Timer* 地址，即使两个任务时间完全一样，它们也是不同的元素
    TimerList timers_;

    // 辅助数据结构：用于 cancel 时通过 Timer* 快速找到 Timer
    // 主要是为了防止地址复用导致的误删（虽然有 sequence 保护）
    ActiveTimerSet activeTimers_;

    // 标记正在处理过期事件，防止在回调中 cancel 导致迭代器失效等问题
    bool callingExpiredTimers_;
    // 保存在定时器到期阶段执行回调的过程中被cancel的定时器（自杀），将来用于在reset阶段标记不进行复活
    ActiveTimerSet cancelingTimers_;
};


#endif //TIMERQUEUE_H
