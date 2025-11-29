//
// Created by user on 2025/11/27.
//

#ifndef TIMER_H
#define TIMER_H
#include <atomic>

#include "Callbacks.h"
#include "base/NonCopyable.h"
#include "base/TimeStamp.h"

/// 定时器类
/// 代表一个单独的定时任务。它需要包含任务回调、过期时间以及唯一的序列号（用于区分同一时间过期的不同任务）
class Timer : NonCopyable {
public:
    Timer(TimerCallback cb, const TimeStamp expiration, const double interval)
        : callback_(std::move(cb)),
          expiration_(expiration),
          interval_(interval),
          repeat_(interval > 0.0),
          sequence_(s_numCreated_.fetch_add(1)) // 原子递增生成序列号
    {}

    /// 对外的接口：执行回调
    /// 调用者应该保证在定时器过期后被执行
    void call_back() const {
        if (callback_) callback_();
    }

    TimeStamp expiration() const { return expiration_; }
    bool repeat() const { return repeat_; }
    int64_t sequence() const { return sequence_; }

    // 如果是重复定时器，重启它（计算下一次过期时间）
    void restart(TimeStamp now);

    static int64_t numCreated() { return s_numCreated_.load(); }

private:
    const TimerCallback callback_;
    TimeStamp expiration_;      // 下一次过期的时间戳
    const double interval_;     // 重复间隔 (秒)，0 表示一次性
    const bool repeat_;         // 是否重复
    const int64_t sequence_;    // 全局唯一序列号

    static std::atomic<int64_t> s_numCreated_; // 静态成员变量，用于生成序列号
};



#endif //TIMER_H
