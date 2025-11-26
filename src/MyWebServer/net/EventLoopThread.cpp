//
// Created by user on 2025/11/25.
//

#include "EventLoopThread.h"

#include "EventLoop.h"
#include "base/utils.h"

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb, const std::string& name)
    : loop_(nullptr),
      exiting_(false),
      init_callback_(cb),
      name_(name)
{
    // 注意：这里构造时还没有启动线程，thread_ 还是空的
}

EventLoopThread::~EventLoopThread()
{
    exiting_ = true;
    // 如果 loop 正在运行，需要通知它退出
    if (loop_ != nullptr) {
        loop_->quit(); // 让loop退出循环
        thread_.join(); // 等待thread_执行结束，其实也就是相当于threadFunc执行完毕
    }
    // 不需要考虑loop_是否应该delete，因为它是对栈上数据的引用，当线程销毁时（它的栈也会被回收），loop_指向的EventLoop会自动回收
}

EventLoop* EventLoopThread::startLoop()
{
    // 启动新线程，执行 threadFunc
    thread_ = std::thread(&EventLoopThread::threadFunc, this);

    {
        // 必须等待子线程将 EventLoop 对象创建完毕，也即等待loop_不为nullptr
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this](){return loop_ != nullptr;});
    }

    // 此时 loop_ 已经被赋值，可以安全返回
    return loop_;
}

/// 子线程核心逻辑
void EventLoopThread::threadFunc()
{
    // 修改线程名
    setCurrentThreadName(name_);

    // 核心：EventLoop 是在子线程的栈上创建的！
    // 它的生命周期与 threadFunc 函数一致
    EventLoop loop;

    // 执行必要的初始化
    if (init_callback_) {
        init_callback_(&loop);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 获取 loop 对象的地址，赋值给成员变量
        loop_ = &loop;
        // 通知 startLoop()，loop 对象已经准备好了
        cond_.notify_one();
    }

    // 开始事件循环，这里会无限循环，直到 call quit()
    loop.loop();

    // loop 结束，清理指针
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
}