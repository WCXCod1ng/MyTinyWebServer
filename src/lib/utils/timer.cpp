//
// Created by user on 2025/11/11.
//

#include "timer.h"

void TimerManager::add_timer(TimerId id, Duration timeout, TimerCallback cb) {
    // 计算超时时间
    Timestamp expires = Clock::now() + timeout;
    // 如果ID已经存在则会覆盖；如果不存在则会插入
    m_timers[id] = TimerNode{expires, std::move(cb)};
}

void TimerManager::adjust_timer(TimerId id, Duration new_timeout) {
    auto it = m_timers.find(id);
    // 查找
    if(it != m_timers.end()) {
        // 更新超时时间
        it->second.expires = Clock::now() + new_timeout;
    }
}

void TimerManager::remove_timer(TimerId id) {
    m_timers.erase(id);
}

void TimerManager::tick() {
    if(m_timers.empty()) {
        return;
    }

    auto now = Clock::now();
    // 要采用两次遍历：这样做是为了避免在遍历过程中修改哈希表（执行回调时可能添加/删除其他定时器）

    // 第一次遍历，找到所有已到期的定时器ID
    std::vector<TimerId> expired_ids;
    for(const auto &[k, v] : m_timers) {
        // 记录过期的定时器的id
        if(now > v.expires) {
            expired_ids.push_back(k);
        }
    }
    // 第二遍：处理所有已到期的定时器
    for(const TimerId id : expired_ids) {
        // 再次搜索的原因还是因为防止其他定时器在执行回调函数时修改了m_timers
        auto it = m_timers.find(id);
        if(it != m_timers.end()) {
            // 执行回调
            it->second.callback();
            // 从哈希表中移除该定时器
            m_timers.erase(it);
        }
    }
}
