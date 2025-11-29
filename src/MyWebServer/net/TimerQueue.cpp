//
// Created by user on 2025/11/27.
//

#include "TimerQueue.h"

#include <cassert>

#include "Timer.h"
#include "TimerId.h"
#include "EventLoop.h"
#include "../log/logger.h"

#include <sys/timerfd.h>
#include <unistd.h>
#include <cstring>

// --- 辅助函数：操作 timerfd ---

int createTimerfd() {
    // CLOCK_MONOTONIC: 单调时钟，不受系统时间修改影响
    // TFD_NONBLOCK: 非阻塞
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0) {
        LOG_ERROR("Failed in timerfd_create");
    }
    return timerfd;
}

/// 重置timerfd的到期时间：计算当前时间与过期时间的差值，设置给 timerfd，以便于timerfd在下一次指定的时间触发IO事件
/// @param timerfd 需要操作的timerfd
/// @param expiration 新的到期时间
void resetTimerfd(const int timerfd, const TimeStamp expiration) {
    struct itimerspec newValue;
    struct itimerspec oldValue;
    std::memset(&newValue, 0, sizeof newValue);
    std::memset(&oldValue, 0, sizeof oldValue);

    // 计算距离现在的微秒数
    int64_t microSeconds = expiration.microSecondsSinceEpoch() - TimeStamp::now().microSecondsSinceEpoch();
    if (microSeconds < 100) {
        microSeconds = 100; // 至少等待 100 微秒
    }

    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(microSeconds / 1000000);
    ts.tv_nsec = static_cast<long>((microSeconds % 1000000) * 1000);

    newValue.it_value = ts;
    // 这里的 0 表示相对时间
    if (::timerfd_settime(timerfd, 0, &newValue, &oldValue)) {
        LOG_ERROR("timerfd_settime()");
    }
}

void readTimerfd(const int timerfd, TimeStamp now) {
    uint64_t howmany;
    // 必须读出数据，否则 LT 模式下会一直触发
    ssize_t n = ::read(timerfd, &howmany, sizeof howmany);
    if (n != sizeof howmany) {
        LOG_ERROR("TimerQueue::handleRead() reads {} bytes instead of 8", n);
    }
}

// --- TimerQueue 实现 ---

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      timerfd_(createTimerfd()),
      timerfdChannel_(loop, timerfd_),
      timers_(),
      callingExpiredTimers_(false)
{
    // 绑定 Channel 回调
    timerfdChannel_.setReadCallback(std::bind(&TimerQueue::handleChannelRead, this));
    // 开启读事件监听
    timerfdChannel_.enableReading();
}

TimerQueue::~TimerQueue() {
    timerfdChannel_.disableAll();
    timerfdChannel_.remove();
    ::close(timerfd_);
    // 清理所有 Timer 内存
    for (const auto& timer : timers_) {
        delete timer.second;
    }
}


TimerId TimerQueue::addTimer(TimerCallback cb, const TimeStamp expiration, const double interval) {
    auto* timer = new Timer(std::move(cb), expiration, interval);

    // 必须在 Loop 线程中修改 timers_ 集合
    loop_->runInLoop([this, timer]() {
        // 插入并检查是否是最早的一个
        bool earliestChanged = insert(timer);
        if (earliestChanged) {
            // 如果新插入的这个是最早过期的（它的超时时间要比之前已插入的都要早），需要重置 timerfd 的唤醒时间
            // 因为我们只需要维护timerfd的到期时间是定时器队列中最早到期的那个的超时时间
            resetTimerfd(timerfd_, timer->expiration());
        }
    });

    return {timer, timer->sequence()};
}

void TimerQueue::cancel(TimerId timerId) {
    loop_->runInLoop([this, timerId]() {
        Entry entry(timerId.timer_->expiration(), timerId.timer_);
        ActiveTimer activeTimer(timerId.timer_, timerId.sequence_);

        auto it = activeTimers_.find(activeTimer);
        if (it != activeTimers_.end()) {
            // 找到了，直接删除
            size_t n = timers_.erase(entry);
            assert(n == 1); (void)n;
            delete it->first; // 释放 Timer 内存
            activeTimers_.erase(it);
        } else if (callingExpiredTimers_) {
            // 如果正在执行回调过程中取消了某个定时器（可能是自杀）
            // 此时 activeTimers_ 里可能没有它了（因为它已经过期被移出来了）
            // 我们需要记下来，在 reset 阶段不要让它“复活”
            // 不需要再这里释放Timer的内存，而是等到reset阶段释放
            cancelingTimers_.insert(activeTimer);
        }
    });
}

void TimerQueue::handleChannelRead() {
    loop_->assertInLoopThread();
    TimeStamp now(TimeStamp::now());

    readTimerfd(timerfd_, now); // 从epoll中读出这个结果

    // 1. 获取所有过期的定时器
    std::vector<Entry> expired = getExpired(now);

    // 开始处理过期事件
    callingExpiredTimers_ = true;
    cancelingTimers_.clear();

    // 2. 执行过期的定时器所携带的回调
    for (const auto&[_, timer] : expired) {
        timer->call_back();
    }

    // 过期事件处理完毕
    callingExpiredTimers_ = false;

    // 3. 重置重复定时器
    reset(expired, now);
}

std::vector<TimerQueue::Entry> TimerQueue::getExpired(TimeStamp now) {
    std::vector<Entry> expired;

    // 哨兵值：查找 >= (now + 0.00...1) 的第一个元素
    // UINTPTR_MAX 保证了同时间的 Timer 也会被包含进来
    Entry sentry(now, reinterpret_cast<Timer*>(UINTPTR_MAX));

    // lower_bound 返回第一个 >= sentry 的迭代器
    // 所以 begin 到 end 之间的都是 < sentry 的（即已过期的）
    const auto end = timers_.lower_bound(sentry);

    // 将过期的从 set 中拷贝出来
    std::copy(timers_.begin(), end, std::back_inserter(expired));

    // 从 set 中移除
    timers_.erase(timers_.begin(), end);

    // 也要从 activeTimers_ 中移除
    for (const Entry& entry : expired) {
        ActiveTimer timer(entry.second, entry.second->sequence());
        size_t n = activeTimers_.erase(timer);
        assert(n == 1); (void)n;
    }

    return expired;
}

void TimerQueue::reset(const std::vector<Entry>& expired, const TimeStamp now) {
    for (const auto&[_, expired_timer] : expired) {
        ActiveTimer active_timer(expired_timer, expired_timer->sequence());

        // 如果是重复定时器，并且没有被取消（这里的意思是在执行定时回调的过程中没有执行cancel）
        if (expired_timer->repeat() && !cancelingTimers_.contains(active_timer)) {
            // 重新启动
            expired_timer->restart(now);
            // 重新放回 set
            insert(expired_timer);
        } else {
            // 一次性定时器，或者已被取消，直接删除内存
            delete expired_timer;
        }
    }

    // 如果队列不为空，需要重置 timerfd 为队头的时间
    // 这里不需要判断是因为传入的expired已经是过期的了（也就是说它们已经被从队列中移除了），那么队首一定会发生变化
    if (!timers_.empty()) {
        const TimeStamp nextExpire = timers_.begin()->first;
        resetTimerfd(timerfd_, nextExpire);
    }
}

bool TimerQueue::insert(Timer* timer) {
    loop_->assertInLoopThread();
    bool earliestChanged = false;

    TimeStamp when = timer->expiration();
    auto it = timers_.begin();

    // 如果 set 是空的，或者新插入的时间比最早的还要早
    if (it == timers_.end() || when < it->first) {
        earliestChanged = true;
    }

    // 插入 timers_
    {
        std::pair<TimerList::iterator, bool> result = timers_.emplace(when, timer); // 原地构造插入
        assert(result.second); (void)result;
    }

    // 插入 activeTimers_
    {
        std::pair<ActiveTimerSet::iterator, bool> result
            = activeTimers_.emplace(timer, timer->sequence());
        assert(result.second); (void)result;
    }

    return earliestChanged;
}