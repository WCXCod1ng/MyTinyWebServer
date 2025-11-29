//
// Created by user on 2025/11/25.
//

#ifndef EVENTLOOP_H
#define EVENTLOOP_H
#include <functional>
#include <thread>

#include "Callbacks.h"
#include "TimerId.h"
#include "base/NonCopyable.h"
#include "base/TimeStamp.h"

class Channel;
class EpollPoller;
class TimerQueue;

/**
 * @brief 事件循环类 (One Loop Per Thread)
 * 核心职责：
 * 1. 持续循环，通过 Poller 获取活跃事件
 * 2. 分发事件给 Channel 处理
 * 3. 执行其他线程塞入的“回调任务” (Pending Functors)
 */
class EventLoop : NonCopyable {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    // 定时器相关的接口

    /// 在指定时间执行
    /// @param time 执行回调的时间点
    /// @param cb 需要执行的回调
    TimerId runAt(TimeStamp time, TimerCallback cb);

    /// 在delay秒后执行回调
    /// @param delay 延迟执行时间
    /// @param cb 需要执行的回调
    TimerId runAfter(double delay, TimerCallback cb);

    /// 每个interval秒执行一次
    /// @param interval 周期执行的间隔
    /// @param cb 需要执行的回调
    TimerId runEvery(double interval, TimerCallback cb);

    /// 取消指定的定时器
    /// @param timerId 需要取消的定时器的句柄
    void cancel(TimerId timerId);

    // 开启事件循环 (必须在创建对象的线程中调用)
    void loop();

    // 退出事件循环
    void quit();

    // --- 核心线程安全接口 ---

    // 立即在当前 Loop 线程执行 cb
    // 如果当前就是 Loop 线程，直接执行；否则调用 queueInLoop
    void runInLoop(Functor cb);

    // 把 cb 放入队列，唤醒 Loop 线程执行
    void queueInLoop(Functor cb);

    // --- 内部接口 ---

    //唤醒 Loop 所在线程 (向 eventfd 写数据)
    void wakeup();

    // Poller 的代理方法，对它的调用实际上最终会被Poller执行
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
    bool hasChannel(Channel* channel);

    // 判断当前线程是否是 Loop 所在线程
    [[nodiscard]] bool isInLoopThread() const { return threadId_ == std::this_thread::get_id(); }

    // 断言
    void assertInLoopThread() {
        if (!isInLoopThread()) {
            abortNotInLoopThread();
        }
    }

private:
    // 处理 wakeupfd 的读事件（读取 8 字节，防止电平触发模式下反复触发）
    void handleWakeupChannelRead();
    // 执行队列中的回调函数
    void doPendingFunctors();
    void abortNotInLoopThread();

    using ChannelList = std::vector<Channel*>;

    std::atomic<bool> looping_;  // 是否正在循环
    std::atomic<bool> quit_;     // 是否退出标识

    const std::thread::id threadId_; // 记录创建 Loop 的线程 ID

    // 核心组件：Poller
    std::unique_ptr<EpollPoller> poller_;

    // --- Wakeup 机制相关 ---
    int wakeupFd_; // eventfd，用于唤醒 epoll_wait
    std::unique_ptr<Channel> wakeupChannel_;

    // --- 活跃通道 ---
    ChannelList activeChannels_; // Poller 返回的活跃通道

    // --- 任务队列 ---
    std::mutex mutex_; // 保护 pendingFunctors_
    // 存储其他线程需要本 Loop 执行的任务
    std::vector<Functor> pendingFunctors_;
    // 标识当前是否正在执行等待队列中的任务
    std::atomic<bool> callingPendingFunctors_;

    /// 定时器队列（持有）
    std::unique_ptr<TimerQueue> timerQueue_;
};


#endif //EVENTLOOP_H
