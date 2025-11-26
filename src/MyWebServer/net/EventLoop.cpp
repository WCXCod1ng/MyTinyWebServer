//
// Created by user on 2025/11/25.
//

#include "EventLoop.h"
#include <sys/eventfd.h>
#include <thread>
#include <format>
#include "Channel.h"
#include "EpollPoller.h"
#include "log/logger.h"
// 防止一个线程创建多个 EventLoop
// __thread 是 GCC 内置的线程局部存储，C++11 推荐用 thread_local
thread_local EventLoop* t_loopInThisThread = nullptr;

// 定义默认的 Poller 超时时间 (虽然你的 Poller 用了 -1，但这里保留接口弹性)
const int kPollTimeMs = 10000;

// 创建 eventfd，用于唤醒阻塞中的Poller
int createEventfd() {
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0) {
        LOG_ERROR("Failed in eventfd");
        abort();
    }
    return evtfd;
}

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      threadId_(std::this_thread::get_id()),
      poller_(std::make_unique<EpollPoller>(this)),
      wakeupFd_(createEventfd()),
      wakeupChannel_(std::make_unique<Channel>(this, wakeupFd_)),
      callingPendingFunctors_(false)
{
    std::stringstream ss;
    ss << threadId_;
    LOG_DEBUG("EventLoop created in thread {}", ss.str()); // 假设 Log 支持这种格式

    if (t_loopInThisThread) {
        LOG_ERROR("Another EventLoop exists in this thread");
        abort(); // 一个线程只能有一个 Loop
    } else {
        t_loopInThisThread = this;
    }

    // 设置 wakeupChannel 的事件类型，并注册读回调
    // 监听读事件 -> 当 queueInLoop 调用 wakeup() 写数据时，这里被触发
    wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead, this));
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop()
{
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    ::close(wakeupFd_);
    t_loopInThisThread = nullptr;
}

void EventLoop::loop()
{
    assertInLoopThread();
    looping_ = true;
    quit_ = false;

    LOG_INFO("EventLoop start looping");

    while (!quit_) {
        activeChannels_.clear();

        // 1. 调用 Poller 等待事件
        // 你的 Poller::poll 返回 vector<Channel*>
        activeChannels_ = poller_->poll();

        TimeStamp receiveTime = TimeStamp::now();

        // 2. 处理活跃事件
        for (Channel* channel : activeChannels_) {
            // channel->handleEvent(TimeStamp::now());
            channel->handleEvent(receiveTime);
        }

        // 3. 执行其他线程排队的任务
        // 必须在处理完事件后执行，否则如果 epoll 阻塞，这些任务就无法执行
        doPendingFunctors();
    }

    LOG_INFO("EventLoop stop looping");
    looping_ = false;
}

void EventLoop::quit()
{
    quit_ = true;
    // 如果是在其他线程调用 quit，需要唤醒 Loop 线程让它从 poll 中返回
    // 才能执行到 while(!quit_) 的判断
    if (!isInLoopThread()) {
        wakeup();
    }
}

/// 在Loop线程里执行cb这个函数，同时它也实现了Dispatch的逻辑（主线程提交一个任务给子EventLoop）
/// 如果当前调用者就是Loop线程自己：直接执行cb
/// 如果当前调用者是其他线程（不是执行EventLoop的线程），则需要调用queueInLoop进行排队
void EventLoop::runInLoop(Functor cb)
{
    if (isInLoopThread()) {
        cb();
    } else {
        queueInLoop(std::move(cb));
    }
}

/// 将任务放入等待队列，并唤醒Loop
void EventLoop::queueInLoop(Functor cb)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(std::move(cb));
    }

    // 为什么要唤醒？ 因为 Loop 可能正阻塞在 epoll_wait。如果不唤醒，它不知道有新任务来了，这个任务可能要等很久（直到有网络数据来）才会被执行。
    // 唤醒条件：
    // 1. 调用 queueInLoop 的不是 Loop 线程本身（需要在 epoll_wait 返回后执行）
    // 2. 或者 Loop 线程正在执行 doPendingFunctors（此时又有了新任务，需要再次唤醒，否则下一轮循环可能再次阻塞在 poll）
    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();
    }
}

/// 往 wakeupFd_（一个 eventfd）里写 8 个字节。这会让 epoll_wait 监听到读事件，从而立即返回
void EventLoop::wakeup()
{
    uint64_t one = 1;
    // 因为我们已经将wakeupFd注册到Epoller上了，所以一旦向其中写入数据，它就会导致Epoller中的epoll_wait立即被唤醒。同时执行一个callback：handleRead()
    // note 计数器机制：eventfd 本质是一个内核维护的 64 位无符号整数计数器。
    // Write：向其写入 8 字节整数，内核会把这个值 累加 到计数器上。这个操作是原子的。内核保证一次 8 字节的写入要么全部成功，要么全部失败，不存在“只写入了 4 个字节”的情况。
    // Read：从其读取 8 字节，内核会把计数器的值读出来，并将计数器 清零（默认模式）。同样，这也是一次性完成的。
    // 原始项目的代码使用TCP方式对其进行了封装，在这里是不需要的，因为内核可以保证原子性
    ssize_t n;
    // 使用 do-while 处理 EINTR，这是 Linux 系统编程的标准范式
    // 如果被信号打断，不仅不报错，而是直接重试
    do {
        n = ::write(wakeupFd_, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);

    if (n != sizeof(one)) {
        // 这里如果是 EAGAIN 也不应该发生，因为 wakeupFd 虽是 non-blocking，
        // 但 eventfd 只有在计数器溢出(2^64-2)时才会 block/EAGAIN，唤醒场景不可能溢出。
        LOG_ERROR("EventLoop::wakeup() writes {} bytes instead of 8, errno={}", n, errno);
    }
}

/// 从 wakeupFd_ 里把那 8 个字节读出来。如果不读，下一次 epoll_wait 会认为还有数据，导致忙轮询（Busy Loop）
void EventLoop::handleRead()
{
    uint64_t one = 1;
    ssize_t n;
    do {
        n = ::read(wakeupFd_, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);

    if (n != sizeof(one)) {
        // 这里可能会出现 EAGAIN (如果是 ET 模式且被虚假唤醒)，这种情况下可以忽略错误
        if (errno != EAGAIN) {
            LOG_ERROR("EventLoop::handleRead() reads {} bytes instead of 8, errno={}", n, errno);
        }
    }
}

void EventLoop::updateChannel(Channel* channel)
{
    assertInLoopThread(); // 必须在 Loop 线程操作 Poller
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel)
{
    assertInLoopThread();
    poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel* channel)
{
    return poller_->hasChannel(channel);
}

void EventLoop::abortNotInLoopThread()
{
    LOG_ERROR("EventLoop::abortNotInLoopThread - EventLoop was created in threadId_ = ... current thread id = ...");
    // 实际项目中可以打印具体的 thread ID
    abort();
}

/// 用于消费任务队列
/// 它不直接持有锁执行任务，而是先把 pendingFunctors_ 里的东西 swap 到局部变量里，释放锁，然后再执行。这样可以大大减小锁的粒度，避免死锁
void EventLoop::doPendingFunctors()
{
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;

    // 关键优化：为了防止死锁和减小锁范围
    // 我们把 pendingFunctors_ 倒换到局部变量 functors 中
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }

    // 此时已释放锁，可以安全执行回调
    for (const auto& functor : functors) {
        functor();
    }

    callingPendingFunctors_ = false;
}