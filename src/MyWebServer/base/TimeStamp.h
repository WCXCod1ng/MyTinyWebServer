//
// Created by user on 2025/11/25.
//

#ifndef TIMESTAMP_H
#define TIMESTAMP_H

#include <iostream>
#include <string>
#include <chrono>

/// 使用 std::chrono 封装，但为了网络编程的性能（如在红黑树或堆中排序），内部保留 int64_t 的微秒数
class TimeStamp {
public:
    TimeStamp() : micro_seconds_since_epoch_(0) {}
    explicit TimeStamp(int64_t microSecondsSinceEpoch) : micro_seconds_since_epoch_(microSecondsSinceEpoch) {}

    static TimeStamp now() {
        return TimeStamp(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    // 方便比较
    bool operator<(const TimeStamp& rhs) const { return micro_seconds_since_epoch_ < rhs.micro_seconds_since_epoch_; }
    bool operator==(const TimeStamp& rhs) const { return micro_seconds_since_epoch_ == rhs.micro_seconds_since_epoch_; }

    [[nodiscard]] std::string toString() const; // 格式化为 "20250101 12:00:00.123456"

    [[nodiscard]] int64_t microSecondsSinceEpoch() const { return micro_seconds_since_epoch_; }

private:
    int64_t micro_seconds_since_epoch_;
};


inline std::string TimeStamp::toString() const {
    char buf[128] = {0};
    auto seconds = static_cast<time_t>(micro_seconds_since_epoch_ / 1000000);
    auto microseconds = static_cast<int>(micro_seconds_since_epoch_ % 1000000);

    struct tm tm_time;
    localtime_r(&seconds, &tm_time);

    snprintf(buf, sizeof(buf), "%4d%02d%02d %02d:%02d:%02d.%06d",
             tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
             tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec,
             microseconds);
    return {buf};
}

#endif //TIMESTAMP_H
