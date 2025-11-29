//
// Created by user on 2025/11/27.
//

#include "Timer.h"
std::atomic<int64_t> Timer::s_numCreated_(0);

void Timer::restart(TimeStamp now) {
    if (repeat_) {
        // 计算新的过期时间：当前时间 + 间隔
        // 这里假设 TimeStamp 有 addTime 辅助函数，或者我们手动计算
        // 假设 TimeStamp 内部是微秒
        int64_t delta = static_cast<int64_t>(interval_ * 1000000);
        expiration_ = TimeStamp(now.microSecondsSinceEpoch() + delta);
    } else {
        expiration_ = TimeStamp::invalid(); // 设置为无效
    }
}