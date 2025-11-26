//
// Created by user on 2025/11/25.
//

#ifndef EPOLLPOLLER_H
#define EPOLLPOLLER_H
#include <map>

#include "base/NonCopyable.h"
#include "base/TimeStamp.h"


class Channel;
class EventLoop;

/**
 * @brief EpollPoller
 * 职责：IO 多路复用的核心实现，封装 Linux epoll。
 *
 * 只有 EventLoop 拥有 Poller 的实例，且生命周期与 EventLoop 一致。
 * 所以 Poller 内部不需要使用 shared_ptr 来管理 Channel，
 * 因为 Channel 的生命周期由 TcpConnection 持有，肯定比 Poller 长（在处理事件时）。
 */
class EpollPoller : NonCopyable {
public:
    using ChannelList = std::vector<Channel*>;

    explicit EpollPoller(EventLoop* loop);
    ~EpollPoller();

    // 核心 IO 复用接口：等待事件发生
    ChannelList poll();

    // 更新 Channel 的关注事件 (ADD/MOD/DEL 的逻辑判断在此)
    // 根据 Channel 的 index_（kNew, kAdded, kDeleted）决定操作类型。
    // kNew (未添加): 调用 epoll_ctl(EPOLL_CTL_ADD)，将 Channel 加入 map。
    // kAdded (已添加): 调用 epoll_ctl(EPOLL_CTL_MOD)，更新感兴趣的事件。
    // kDeleted (已删除): 类似 kNew，重新 ADD 进去（逻辑上复用）。
    void updateChannel(Channel* channel);

    // 从 Poller 中移除 Channel
    void removeChannel(Channel* channel);

    // 判断 Channel 是否被当前 Poller 监控
    bool hasChannel(Channel* channel) const;

private:
    // 默认监听事件列表的大小
    static const int kInitEventListSize = 16;

    // 填写活跃的 Channel
    ChannelList getActiveChannels(int numEvents) const;

    // 执行真正的 epoll_ctl
    // 这是对系统调用 epoll_ctl 的最底层封装。
    // 在这里，我们直接调用们将 channel 指针赋值给 epoll_event.data.ptr。这是 Reactor 模型中将 C++ 对象与 OS 事件关联的关键一步
    void update(int operation, Channel* channel);

    using EventList = std::vector<struct epoll_event>;
    using ChannelMap = std::map<int, Channel*>;

    // 这是 epoll_create1(EPOLL_CLOEXEC) 返回的文件描述符。
    // 整个 Poller 生命周期都围绕这个 fd 运转。析构时需要 close(epollfd_)。
    int epollfd_;       // epoll_create1 返回的句柄
    // 传给 epoll_wait 的“输出参数”。当 epoll_wait 返回时，内核会把发生的事件填充到这个 vector 的底层数组里
    EventList events_;  // 用于接收 epoll_wait 返回的事件
    // 维护当前 EventLoop 中所有的 Channel
    // 记录 fd -> Channel* 的映射，用于保活校验
    ChannelMap channels_;
    // 所属的 EventLoop
    EventLoop* ownerLoop_;
};


#endif //EPOLLPOLLER_H
