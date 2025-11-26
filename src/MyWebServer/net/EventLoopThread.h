//
// Created by user on 2025/11/25.
//

#ifndef EVENTLOOPTHREAD_H
#define EVENTLOOPTHREAD_H
#include <condition_variable>
#include <functional>
#include <thread>

#include "base/NonCopyable.h"

class EventLoop;

/**
 * @brief IO 线程类
 *
 * 这个类做了两件事：
 * 1. 启动一个 std::thread
 * 2. 在那个线程里创建一个 EventLoop 对象，并让它 loop() 起来
 */
class EventLoopThread : NonCopyable {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    /// 时间顺序：EventLoopThread 构造 -> startLoop -> threadFunc -> EventLoop 构造
    EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback(),
                    const std::string& name = std::string());
    ~EventLoopThread();

    // 启动线程，并阻塞等待，直到 loop 对象创建成功
    EventLoop* startLoop();

private:
    // 线程入口函数
    void threadFunc();

    // 指向该线程栈上的 EventLoop 对象
    // 并不代表拥有该loop，而是一个弱引用
    EventLoop* loop_;
    bool exiting_;

    // 与EventLoop绑定的线程，它负责执行loop
    std::thread thread_;
    // 该线程的名称
    std::string name_;

    std::mutex mutex_;
    /// 条件变量用于阻塞等待子线程启动完毕（返回一个非空的EventLoop）
    /// 为什么要使用条件变量：当主线程调用Loop时，内部会构造一个thread对象，但是这个过程是与主线程并发的（执行threadFunc，并在内部初始化loop_），有可能还未构造完毕主线程就拿到了结果（这个结果可能是nullptr）
    /// 使用条件变量等待loop_不为nullptr，之后再返回即可
    std::condition_variable cond_;

    /// 线程初始化回调（在 loop 运行前执行）：
    /// 比如你想给每个 IO 线程设置名称（对于调试非常重要），或者你想在 IO
    /// 线程启动前初始化一些线程局部存储（TLS）的数据，就可以传入这个回调
    ThreadInitCallback init_callback_;
};



#endif //EVENTLOOPTHREAD_H
