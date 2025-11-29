//
// Created by user on 2025/11/25.
//
#include "Channel.h"
#include "EventLoop.h"
#include <sys/epoll.h>

// 静态成员变量初始化
const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI; // EPOLLPRI通常用于带外数据，这里视为可读
const int Channel::kWriteEvent = EPOLLOUT;
const int Channel::enableET = EPOLLET;

Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop),
      fd_(fd),
      events_(0),
      revents_(0),
      index_(ChannelStatus::kNew), // -1 代表 kNew，表示该 Channel 未添加到 Poller
      tied_(false)
{
}

Channel::~Channel()
{
    //断言当前 Channel 不是在 loop 中处理事件的状态，或者是没有任何关注事件的状态
    //在析构时不做 close(fd_)，因为 Channel 不拥有 fd，fd 的生命周期由 TcpConnection 或 Socket 管理
}

// 核心逻辑：生命周期保护
void Channel::tie(const std::shared_ptr<void>& obj)
{
    tie_ = obj;
    tied_ = true;
}

// 核心逻辑：通知 EventLoop 更新我们在 epoll 中的状态
void Channel::update()
{
    // 通过 EventLoop 调用 Poller::updateChannel
    // channel->events_ 已经修改了，这里是去通知 Poller 做出相应的 epoll_ctl 更改
    loop_->updateChannel(this);
}

// 核心逻辑：将自己从 EventLoop 中移除
void Channel::remove()
{
    // 通过 EventLoop 调用 Poller::removeChannel
    loop_->removeChannel(this);
}

/// 核心逻辑：事件分发入口
/// 1. 为什么需要 tie 和 handleEvent 的两层设计？ (面试高频考点)
/// 这是为了解决 TcpConnection 生命周期的竞态条件 (Race Condition)。
/// - 场景：用户断开连接，TcpConnection 及其拥有的 Channel 正准备析构。
/// - 问题：与此同时，EventLoop 的 epoll_wait 刚好返回了这个 fd 的事件。
/// - 后果：如果没有保护，EventLoop 接下来会调用 channel->handleEvent()。此时如果 Channel 或其依附的 TcpConnection 已经被销毁了，程序就会崩溃（Core Dump）。
/// - 解决方案 (tie)：
///     - 在 TcpConnection 创建时，把自己（shared_ptr<TcpConnection>）绑定到 Channel 的 weak_ptr tie_ 上。
///     - 当 Channel::handleEvent 被调用时，先尝试 tie_.lock()。
///     - 如果 TcpConnection 还在，lock() 会返回有效的 shared_ptr，引用计数 +1，保证在处理完这次事件之前，TcpConnection 绝对不会被析构。
///     - 如果 TcpConnection 已经没了，lock() 返回空，Channel 就静默忽略这次事件，从而安全退出。
void Channel::handleEvent(TimeStamp receiveTime)
{
    // 如果设置了 tie_，则需要延长生命周期
    if (tied_) {
        // 尝试提升 weak_ptr 为 shared_ptr
        std::shared_ptr<void> guard = tie_.lock();
        if (guard) {
            // 只有当对象（通常是 TcpConnection）还活着时，才处理事件
            handleEventWithGuard(receiveTime);
        } else {
            // guard 为空说明 TcpConnection 已经析构了，忽略该事件，防止 core dump
            // LOG_DEBUG << "Channel::handleEvent - TcpConnection already destroyed";
        }
    } else {
        handleEventWithGuard(receiveTime);
    }
}

/// 核心逻辑：根据 revents_ 具体的事件类型，调用对应的回调函数——一个Dispatcher
/// Linux epoll 返回的事件标志位 (revents) 经常是混合的，处理逻辑有讲究，如下是muduo库的经典做法：
/// - EPOLLHUP (挂断)：通常表示 TCP 连接被对端重置或关闭。如果在没有 EPOLLIN 的情况下收到 HUP，我们直接调用 closeCallback_。
/// - EPOLLERR (错误)：Socket 发生错误，调用 errorCallback_。
/// - EPOLLIN | EPOLLPRI | EPOLLRDHUP (读)：
///   - EPOLLIN: 普通数据可读。
///   - EPOLLPRI: 带外数据（Out-of-band data），虽然现代网络编程很少用，但作为健壮的库通常一并处理。
///   - EPOLLRDHUP: Linux 2.6.17 后引入，专门用于检测对端关闭连接（半关闭），可以减少一次系统调用。这里我们把它一并视为“有动静”，交给 readCallback_ 去处理（也就是去 read，read 返回 0 自然就知道是关闭了）。
/// - EPOLLOUT (写)：内核缓冲区有空闲空间了，可以继续发送数据了，调用 writeCallback_。
void Channel::handleEventWithGuard(TimeStamp receiveTime)
{
    // LOG_INFO << "channel handleEvent revents: " << revents_;

    // 1. EPOLLHUP表示读半部和写半部都已经被关闭（完全关闭），属于真正的关闭
    // 注意：EPOLLHUP 经常和 EPOLLIN 一起出现，当一起出现时表明虽然连接关闭，但是还有残留的可读数据，那么不应当关闭，而是归类为可读（3）；只有“只有 EPOLLHUP 没有 EPOLLIN”时，才认为是异常挂起，直接关闭
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        LOG_WARN("Channel::handleEventWithGuard EPOLLHUP fd = {}", fd_);
        if (closeCallback_) closeCallback_();
    }

    // 2. 处理错误 (EPOLLERR)
    if (revents_ & EPOLLERR) {
        LOG_ERROR("Channel::handleEventWithGuard EPOLLERR fd = {}", fd_);
        if (errorCallback_) errorCallback_();
    }

    // 3. 处理可读 (EPOLLIN) / 高优先级数据 (EPOLLPRI) / 对端关闭写 (EPOLLRDHUP)
    // 注意：EPOLLRDHUP表明对端调用了close()或shutdown(SHUT_WR)，对端不能再发送数据了；但是本端仍然可以继续发送数据（读则会返回0，即EOF）
    // 这里的意思是交给读回调的read()/recv()处理：（1）如果返回>0，正常数据；（2）如果返回0，对端关闭写方向（可能半关闭、也可能全关闭）；（3）如果返回-1+EAGAIN，暂时无数据
    if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        LOG_INFO("有数据可读");
        if (readCallback_) readCallback_(receiveTime);
    }

    // 4. 处理可写 (EPOLLOUT)
    if (revents_ & EPOLLOUT) {
        LOG_INFO("有数据可写");
        if (writeCallback_) writeCallback_();
    }
}