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


/// 为什么要为Channel设置三种状态，目的是：实现逻辑删除+延迟物理删除
/// 如果不使用三态机，就意味着在删除时除了需要从map中也删除（因为此时“是否存在在map”中成为了判断是否删除的依据），还需要执行实际的物理删除（TcpConnection::removeConnection被调用），那么它很可能会被复用（相同的fd但映射了一个新的Channel，因为fd就那么多，在高并发场景下一定会被复用）或者即使不复用指针指向的值也是一个无效地址：但因为在注册事件时使用的是一个指向Channel的指针作为event.data，所以activeChannels数组中存储的event.data指向的还是旧的fd对应的*Channel，导致了指针崩溃，所以使用三态机的是为了防止在遍历activeChannels的过程中：（1）修改了map中fd对应的Channel指针（2）指向的内存失效，从而导致map与activeChannels不一致
/// 不使用三态机，会导致如下的问题：
/// 1. epoll_wait 返回 1000 个活跃 fd，其中 fd=7 对应 Channel A
/// 2. EventLoop 开始遍历这 1000 个 Channel，调用 handleEvent()
/// 3. 轮到 Channel A → 执行 readCallback → 发现对端关闭 → 调用 connection->shutdownInLoop()
/// 4. shutdownInLoop() → channel->disableAll() → updateChannel(channel)
/// 5. updateChannel() 发现 events_=0 → 直接 epoll_ctl(EPOLL_CTL_DEL, fd=7)，并且从map中移除掉了它
/// 6. 继续遍历后面的 500 个 Channel……
///    → 灾难发生了：后面的某个 Channel 恰好被分配到原来 Channel A 的内存地址
///    → 或者 activeChannels 向量里还保存着已经 delete 的 Channel* 裸指针
///    → 崩溃！空指针、野指针、double free 随便跳
/// 所以在一次 epoll_wait 返回的活跃事件正在被回调的过程中，ChannelMap中的channel不会在本次遍历activeChannels过程中被删除（对应的channel也不会被析构），而是等到本次遍历结束后才会进行实际的删除
/// 在使用三态机之后，一个正常的流程是：
/// 1. epoll_wait 返回 1000 个事件（包括 fd=7 的 Channel A）
/// 2. EventLoop 开始遍历 activeChannels
/// 3. 处理到 Channel A → read() 返回 0 → 决定关闭连接
/// 4. 调用 channel->disableAll() → events_ = 0 → 表明不关心这个事件了（是逻辑删除！！！）
/// 5. 调用 updateChannel(channel)
///    ├─ 发现当前是 kAdded，但 events_==0
///    ├─ 执行 epoll_ctl(DEL) 把 fd=7 从 epoll 内核删掉
///    ├─ 把状态改为 kDeleted（关键！没有析构！没有从 map 删！）
///    └─ 返回
/// 6. 继续安全地处理后面 500 个 Channel（此时 Channel A 还活着，指针有效）
/// 7. 所有 1000 个事件都处理完毕，activeChannels 遍历结束
/// 8. 下一次 poll 彻底结束
/// 9. 用户代码调用 connection->forceClose() 或在下一次事件循环中调用 removeChannel(channel)
///    ├─ 发现状态是 kDeleted
///    ├─ 从 channels_ map 中 erase(fd)
///    ├─ 状态改为 kNew（可选）
///    └─ 现在才真正 delete Channel 对象
enum class ChannelStatus {
    kNew = -1, // 从未添加过，不在channels_map中，不在epoll内核的红黑树中
    kAdded = 1, // 已添加到epoll中，在channels_map中，在epoll内核的红黑树中
    kDeleted = 2 // 已从epoll删除，必须在channels_map中，不在epoll内核的红黑树中
};

/// Channel作用是：
/// 1. 封装：sockfd、其感兴趣的时间（如 EPOLLIN、EPOLLOUT）、poller实际返回的ready事件
/// 2. 提供回调注册机制，负责为感兴趣的“事件”映射“相关的回调函数”
/// 3. 作为一个Dispatcher，根据ready事件选择合适的回调函数去执行
/// 4. 它不拥有文件描述符（fd），不负责关闭 fd（那是 Socket 或 TcpConnection 的事）。
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
