//
// Created by user on 2025/11/25.
//

#include "EpollPoller.h"

#include <sys/epoll.h>

#include "Channel.h"
#include "log/logger.h"
#include <cstring>

#include "EventLoop.h"

EpollPoller::EpollPoller(EventLoop* loop)
    : epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(kInitEventListSize),
      ownerLoop_(loop)
{
    if (epollfd_ < 0) {
        LOG_ERROR("EpollPoller::EpollPoller create epollfd error");
        // 实际项目中这里可能需要抛出异常或终止
        throw std::runtime_error("EpollPoller::EpollPoller create epollfd error");
    }
}


EpollPoller::~EpollPoller()
{
    if (epollfd_ >= 0) {
        ::close(epollfd_);
    }
}


EpollPoller::ChannelList EpollPoller::poll()
{
    // 这里的 &*events_.begin() 获取 vector 底层数组的首地址
    // C++11 保证 vector 内存是连续的
    while(true) {
        int numEvents = ::epoll_wait(epollfd_, &*events_.begin(),
                             static_cast<int>(events_.size()), -1);

        int saveErrno = errno; // 保存 errno，防止被后续日志操作覆盖
        TimeStamp now(TimeStamp::now());

        if (numEvents > 0) {
            // LOG_DEBUG << numEvents << " events happened";
            auto active_channels = getActiveChannels(numEvents);

            // 策略：如果 vector 满了，说明当前并发很高，扩容两倍
            if (static_cast<size_t>(numEvents) == events_.size()) {
                events_.resize(events_.size() * 2);
            }

            return active_channels;
        } else if (numEvents == 0) {
            LOG_DEBUG("nothing happened");
        } else {
            // EINTR 是被信号中断，不是错误，可以忽略
            if (saveErrno != EINTR) {
                errno = saveErrno;
                LOG_ERROR("EpollPoller::poll() error");
            }
        }
    }
}


EpollPoller::ChannelList EpollPoller::getActiveChannels(const int numEvents) const
{
    ChannelList activeChannels;
    for (int i = 0; i < numEvents; ++i) {
        // epoll_event.data.ptr 在 update() 时被设置成了 Channel*
        auto* channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels.push_back(channel);
    }
    return activeChannels;
}


void EpollPoller::updateChannel(Channel* channel)
{
    // LOG_DEBUG << "fd = " << channel->fd() << " events = " << channel->events() << " index = " << index;

    if (const ChannelStatus index = channel->index(); index == ChannelStatus::kNew || index == ChannelStatus::kDeleted) {
        // 情况 1: 该 Channel 是新的，或者之前被删除过（逻辑删除）
        // 此时内核中已经没有该fd了，（而updateChannel被调用说明又想要重新监听事件了），因此需要添加EPOLL_CTL_ADD
        // 对于kNew状态，说明之前从来没有被注册，那么map中也应当没有，此时应该加入，并且向内核中注册
        // 对于kDeleted状态，此时map中必然存在（因为我们当时删除的时候没有从map中移除），所以只需要向内核中注册
        // 所以一个正常的状态是：kNew -> kAdded -> disableAll() -> kDeleted -> kAdded ...

        int fd = channel->fd();
        if (index == ChannelStatus::kNew) {
            // 如果是 kNew，需要放入 map 中
            // 断言 map 中肯定没有这个 fd
            // channels_[fd] = channel;
            // 严谨写法：
            if(channels_.contains(fd)) {
                LOG_ERROR("fd = {} must exist in channels_", fd);
            }
            channels_[fd] = channel;
        } else {
            // 如果是 kDeleted，说明updateChannel之前被执行过一次了（而且那次是由于channel->disableAll()被执行的原因，造成内核上的相关事件被删除了，但是map没有被删除，所以这里按理来说map中的的fd还对应着相同的channel）
            if(!channels_.contains(fd)) {
                LOG_ERROR("fd = {} must exist in channels_", fd);
            }
            if(channels_[fd] != channel) {
                LOG_ERROR("current channel is not match the one in channels_");
            }
        }

        channel->set_index(ChannelStatus::kAdded);
        update(EPOLL_CTL_ADD, channel);

    } else {
        // 情况 2: 该 Channel 已经在 epoll 中 (kAdded)

        if (channel->isNoneEvent()) {
            // 如果当前 Channel 不感兴趣任何事件了 (events == 0)，也即channel->disableAll()被调用了（这是删除的入口，即只有被加入过才会被删除）
            // 为了性能，我们将其从 epoll 内核中删除 (EPOLL_CTL_DEL)
            // 但是我们并不从map中删除对应的Channel，主要是为了防止出现野指针、迭代器失效、double free问题等；而是标记为 kDeleted 表明逻辑删除
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(ChannelStatus::kDeleted);
        } else {
            // 还有感兴趣的事件，说明只是修改了事件类型 (比如从 Read 变为 Write)
            update(EPOLL_CTL_MOD, channel);
        }
    }
}


void EpollPoller::removeChannel(Channel* channel)
{
    // 保证只能在 IO 线程操作
    ownerLoop_->assertInLoopThread();

    const int fd = channel->fd();
    // LOG_DEBUG << "func=" << __FUNCTION__ << " fd=" << fd;

    // 各种断言，确保 Channel 状态正确
    if (!channels_.contains(fd)) {
        LOG_ERROR("removeChannel: fd not found");
        return;
    }
    if (channels_[fd] != channel) {
        LOG_ERROR("removeChannel: channel mismatch");
        return;
    }
    if (!channel->isNoneEvent()) {
        // 只有没有关注事件的 Channel 才能被移除（安全起见）
        // 实际上在 Close 时，我们会先 disableAll()
        LOG_ERROR("removeChannel: channel is still interested in events");
    }

    // 1. 从 map 中移除
    channels_.erase(fd);

    // 2. 从 epoll 内核中移除
    if (const ChannelStatus index = channel->index(); index == ChannelStatus::kAdded || index == ChannelStatus::kDeleted) {
        // 如果它还在 epoll 里 (kAdded)，或者虽然标记为 deleted 但之前在 map 里
        // 都要确保调用一次 DEL
        if (index == ChannelStatus::kAdded) {
            update(EPOLL_CTL_DEL, channel);
        }
        // 恢复为 kNew 状态
        channel->set_index(ChannelStatus::kNew);
    }
}

bool EpollPoller::hasChannel(Channel *channel) const {
    if(!channel) return false;

    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}

void EpollPoller::update(const int operation, Channel* channel)
{
    struct epoll_event event = {};

    // 核心：设置感兴趣的事件
    event.events = channel->events();
    // note 我们的目的是当epoll_wait返回时，我们能找回那个事件所从属的Channel对象，所以这里将Channel的指针绑定到data.ptr上
    event.data.ptr = channel;

    int fd = channel->fd();
    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0) {
        if (operation == EPOLL_CTL_DEL) {
            // 删除出错通常不需要 fatal，因为可能 socket 已经提前 close 了
            LOG_ERROR("epoll_ctl del error: {}", errno);
        } else {
            // ADD 或 MOD 出错是致命的
            LOG_ERROR("epoll_ctl add/mod error: ", errno);
            throw std::runtime_error("epoll_ctl add/mod error");
        }
    }
}
