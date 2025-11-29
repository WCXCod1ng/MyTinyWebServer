//
// Created by user on 2025/11/25.
//

#ifndef EVENTLOOPTHREADPOOL_H
#define EVENTLOOPTHREADPOOL_H
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/NonCopyable.h"

class EventLoop;
class EventLoopThread;

///  EventLoopThreadPool 是 Reactor 模型中 “Main Reactor + Sub Reactors” 架构的管理者
///  1. 管理：创建并持有所有的 IO 线程 (EventLoopThread)。
///  2. 调度：当新连接到来时，通过 轮询(Round - Robin) 算法选择一个 EventLoop 来接管这个连接。
class EventLoopThreadPool : NonCopyable {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    /**
     * @param baseLoop 主 Reactor (Main Loop)，通常是处理 Accept 的那个 Loop
     * @param name 线程池名称
     */
    explicit EventLoopThreadPool(EventLoop* baseLoop, size_t numThreads = 0, std::string name = "");
    ~EventLoopThreadPool();

    // 设置线程数量 (必须在 start 之前调用)
    void setThreadNum(const size_t numThreads) { numThreads_ = numThreads; }

    // 启动线程池
    // cb: 线程初始化回调，会在每个 Loop 线程启动时执行
    void start(const ThreadInitCallback& initial_callback = ThreadInitCallback());

    // 【核心】负载均衡算法
    // 获取下一个用于处理新连接的 Loop
    // 如果线程池为空，则返回 baseLoop
    EventLoop* getNextLoop();

    // 获取所有 Loop (通常用于统计或广播)
    std::vector<EventLoop*> getAllLoops();

    bool started() const { return started_; }
    const std::string& name() const { return name_; }

private:
    /// 主线程的 Loop ，监听socket的相关事件：即处理新连接
    /// 为什么需要 baseLoop_（而不是放在threads_中一同处理）？ 这是为了支持两种运行模式：
    /// 1. 单线程模式 (Single Reactor)：
    ///   - 用户设置 setThreadNum(0)。
    ///   - threads_ 为空。
    ///   - getNextLoop() 永远返回 baseLoop_。
    ///   - 结果：Accept 和 IO 处理都在同一个线程（主线程）完成。适合连接数少、业务逻辑极快的场景（或者调试用）。
    /// 2. 多线程模式 (Main + Sub Reactors)：
    ///   - 用户设置 setThreadNum(N) (N > 0)。
    ///   - baseLoop_ 只负责 accept 连接。
    ///   - 新连接通过 getNextLoop() 分发给 threads_ 中的某个 Sub Loop。
    ///   - threads_ 中的 Loop 负责连接的 read/compute/write。
    EventLoop* baseLoop_;
    std::string name_;
    bool started_;
    // IO线程的数量（不包括baseLoop）
    size_t numThreads_;

    // 轮询索引，主要是为了实现Round-Robin算法
    int next_;

    // 独占管理所有的 IO 线程，当线程池被析构时，这些线程都会被执行
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    // 缓存所有的 IO EventLoop 指针（为了快速访问，避免频繁解引用线程对象）
    std::vector<EventLoop*> loops_;
};



#endif //EVENTLOOPTHREADPOOL_H
