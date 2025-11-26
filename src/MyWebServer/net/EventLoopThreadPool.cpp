//
// Created by user on 2025/11/25.
//

#include "EventLoopThreadPool.h"

#include <format>
#include <utility>

#include "EventLoop.h"
#include "EventLoopThread.h"

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, const size_t numThreads, std::string name)
    : baseLoop_(baseLoop),
      name_(std::move(name)),
      started_(false),
      numThreads_(numThreads),
      next_(0)
{
}

EventLoopThreadPool::~EventLoopThreadPool()
{
    // unique_ptr 会自动释放 EventLoopThread，
    // EventLoopThread 析构时会 join 线程，
    // 线程结束会导致栈上的 EventLoop 销毁。
    // 所以这里不需要手动做任何清理。
}

void EventLoopThreadPool::start(const ThreadInitCallback& initial_callback)
{
    if(started_) {
        throw std::runtime_error("状态错误");
    }
    // 确保只能由主 Loop 线程调用 start
    baseLoop_->assertInLoopThread();

    // 启动
    started_ = true;

    for (int i = 0; i < numThreads_; ++i) {
        // 为每个线程命名
        std::string real_name = std::format("{}-{}", name_, i);

        // 创建 EventLoopThread 对象
        auto t = std::make_unique<EventLoopThread>(initial_callback, real_name);

        // 启动线程，并获取其中的 Loop 指针
        // 这一步会阻塞，直到线程启动完成
        loops_.push_back(t->startLoop());

        // 移入容器管理
        threads_.push_back(std::move(t));
    }

    // 如果 numThreads_ 为 0，说明是单线程模型
    // 此时唯一的 Loop 就是 baseLoop_，它也负责IO，所以让其调用
    if (numThreads_ == 0 && initial_callback) {
        initial_callback(baseLoop_);
    }
}

/// 不需要为getNextLoop加锁：
/// EventLoopThreadPool 通常由 TcpServer 持有。
/// TcpServer 处理新连接 (Acceptor 回调) 是在 Main Loop (主线程) 中进行的。
/// start() 也是在主线程调用的。
/// 因此，getNextLoop 永远只会在主线程中被调用。这是一个单写者场景，没有竞争，所以不需要互斥锁。
EventLoop* EventLoopThreadPool::getNextLoop()
{
    // 主 Loop 调用此函数分发新连接
    baseLoop_->assertInLoopThread();
    if(!started_) {
        throw std::runtime_error("状态错误");
    }

    EventLoop* loop = baseLoop_;

    // 如果有子线程，使用 Round-Robin (轮询) 选取一个
    if (!loops_.empty()) {
        loop = loops_[next_];
        ++next_;
        if (static_cast<size_t>(next_) >= loops_.size()) {
            next_ = 0;
        }
    }

    // 如果没有子线程 (numThreads_ == 0)，直接返回 baseLoop_
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops()
{
    baseLoop_->assertInLoopThread();
    if(!started_) {
        throw std::runtime_error("状态错误");
    }

    if (loops_.empty()) {
        return std::vector<EventLoop*>(1, baseLoop_);
    } else {
        return loops_;
    }
}