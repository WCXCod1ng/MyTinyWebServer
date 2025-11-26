//
// Created by user on 2025/11/25.
//

#ifndef CHANNEL_H
#define CHANNEL_H

#include <functional>
#include <memory>
#include <sys/epoll.h> // for EPOLLIN, EPOLLOUT...
#include "../base/NonCopyable.h"
#include "../base/TimeStamp.h"
#include "../log/logger.h"

// 前向声明，为了解耦，Channel 只需要知道 EventLoop 类的存在即可
class EventLoop;

enum class ChannelStatus {
    kNew = -1, // 从未添加过
    kAdded = 1, // 已添加到epoll中
    kDeleted = 2 // 已从epoll删除
};

/// Channel 理解为通道，封装了 sockfd 和其感兴趣的 event，如 EPOLLIN、EPOLLOUT 事件
/// 还绑定了 poller 返回的具体事件
/// - 它不拥有文件描述符（fd），不负责关闭 fd（那是 Socket 或 TcpConnection 的事）。
/// - 它只负责 “映射”：它把一个 fd 和它感兴趣的 IO 事件（读、写、错误）以及事件发生时对应的 回调函数 绑定在一起。
/// - 如果没有 Channel，EpollPoller 就不知道该监听谁，EventLoop 也不知道事件发生后该调谁的函数。
class Channel : NonCopyable {
public:
    using EventCallback = std::function<void()>;
    using ReadEventCallback = std::function<void(TimeStamp time_stamp)>;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    // fd 处理事件的核心入口，由 EventLoop::loop() 调用
    // receiveTime: 事件发生的时间（主要用于计算延迟等）
    void handleEvent(TimeStamp receiveTime);

    // 设置回调函数对象
    void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); }
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    // 防止当 Channel 被手动 remove 后，Channel 还在执行回调导致崩溃
    // 通常在 TcpConnection 建立时调用，绑定 shared_ptr
    void tie(const std::shared_ptr<void>&);

    int fd() const { return fd_; }
    int events() const { return events_; }

    // 设置 revents，这是由 Poller 设置的，表示当前发生的事件
    void set_revents(uint32_t revt) { revents_ = revt; }

    // 判断是否无事件
    // 逻辑：检查是否有“读”或“写”事件。
    // 如果 events_ 里只剩下一个 enableET (EPOLLET) 而没有 IN/OUT，我们也视为 NoneEvent
    bool isNoneEvent() const { return (events_ & (kReadEvent | kWriteEvent)) == 0; }

    // --- 修改感兴趣的事件 ---
    void enableReading() { events_ |= (kReadEvent | enableET); update(); }
    void disableReading() { events_ &= ~kReadEvent; update(); }
    void enableWriting() { events_ |= (kWriteEvent | enableET); update(); }
    void disableWriting() { events_ &= ~kWriteEvent; update(); }
    void disableAll() { events_ = kNoneEvent; update(); }

    // 当前是否关注了写/读事件
    bool isWriting() const { return events_ & kWriteEvent; }
    bool isReading() const { return events_ & kReadEvent; }

    /// for Poller
    /// 表示该 Channel 在 Poller 中的状态（如 kNew, kAdded, kDeleted）
    ChannelStatus index() { return index_; }
    void set_index(const ChannelStatus idx) { index_ = idx; }

    // 获取所属的 EventLoop
    EventLoop* ownerLoop() { return loop_; }

    // 移除当前 Channel
    void remove();

private:
    // 通知 EventLoop -> Poller 更新自己在 epoll 中的状态
    void update();

    // 具体的事件处理逻辑
    void handleEventWithGuard(TimeStamp receiveTime);

    // 静态常量，表示 epoll 的事件标志
    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;
    static const int enableET; // 是否开启ET模式

    EventLoop* loop_; // Channel 所属的 EventLoop
    const int fd_;    // Channel 负责的文件描述符，但不拥有它
    uint32_t events_;      // 注册感兴趣的事件
    uint32_t revents_;     // Poller 返回的实际发生的事件
    ChannelStatus index_;       // Used by Poller，该 Channel 在 Poller 中的状态（如 kNew, kAdded, kDeleted）

    std::weak_ptr<void> tie_; // 弱指针，指向 TcpConnection，用于保活
    bool tied_;

    // 事件回调函数，由上层（TcpConnection/Acceptor）注册
    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};
#endif //CHANNEL_H
