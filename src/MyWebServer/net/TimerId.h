//
// Created by user on 2025/11/27.
//

#ifndef TIMERID_H
#define TIMERID_H
#include <cstdint>

class Timer;

/// 用户持有的定时器句柄，仅包含 Timer 指针和其序列号
/// 不拥有 Timer 的生命周期（使用一个裸指针表示）
class TimerId {
public:
    TimerId() : timer_(nullptr), sequence_(0) {}
    TimerId(Timer* timer, const int64_t seq)
        : timer_(timer), sequence_(seq) {}

    // 友元类，TimerQueue 需要访问私有成员来做对比
    friend class TimerQueue;

    TimerId(const TimerId&) = default;
    TimerId& operator=(const TimerId&) = default;
    TimerId(TimerId&&) = default;
    TimerId& operator=(TimerId&&) = default;

    // 判断当前TimerId是否是悬垂的（因为刚开始建立连接可能是dangling的）
    [[nodiscard]] inline bool dangling() const {
        return timer_ == nullptr;
    }

private:
    Timer* timer_ {nullptr}; // 一个常量指针，指向对应的Timer
    int64_t sequence_ {0}; // 用于区分相同时间到期的定时器，它的值应该与timer_->sequence_一致
};

#endif //TIMERID_H
