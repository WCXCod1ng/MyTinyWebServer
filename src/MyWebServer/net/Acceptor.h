//
// Created by user on 2025/11/25.
//

#ifndef ACCEPTOR_H
#define ACCEPTOR_H
#include <functional>

#include "Channel.h"
#include "Socket.h"
#include "base/NonCopyable.h"

class EventLoop;
class InetAddress;

/**
 * @brief 接收器
 * 职责：
 * 1. 封装 listenfd
 * 2. 关注 listenfd 的读事件 (EPOLLIN)
 * 3. 运行在 mainLoop 中
 * 4. accept 获取新连接，并回调给 TcpServer
 */
class Acceptor : NonCopyable {
public:
    // 回调函数签名：(新连接的fd, 对方的地址)
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress&)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport);
    ~Acceptor();

    // 设置新连接回调（通常由 TcpServer 设置）
    void setNewConnectionCallback(const NewConnectionCallback& cb) {
        newConnectionCallback_ = cb;
    }

    // 开始监听（开启 socket 的 listen，并向 Poller 注册可读事件）
    bool listenning() const { return listenning_; }
    void listen();

private:
    /// 当 listenfd 有读事件发生时（即有新连接连入），Channel 会回调此函数
    void handleRead();

    EventLoop* loop_; // Acceptor 用的就是用户定义的那个 baseLoop
    /// 服务端的监听套接字（Listening Socket）
    /// RAII 管理 listenfd
    /// 注意，它必须是非阻塞的（Non-blocking），这是 Reactor 模型的基石
    Socket acceptSocket_;
    /// 它是连接 acceptSocket_ 和 EventLoop 的桥梁。
    /// 它关注 EPOLLIN 事件。当内核完成三次握手，全连接队列里有新连接时，它会通知 Acceptor 执行 handleRead()。
    /// note 监听socket通常采用 LT (Level Trigger, 水平触发) 模式。
    /// 原因：1. ET模式带来的性能他提升不大 2. 如果用 ET 模式，当并发量极大时，一次 accept 可能处理不完队列里所有的握手请求，剩下的请求如果不处理，下次就不会触发事件了（直到有新数据），导致新连接卡死。虽然可以用 while(accept) 循环处理，但 LT 模式更简单稳健——只要队列里还有连接，epoll 就会一直通知你
    Channel acceptChannel_; // 用于观察 listenfd 的事件

    /// 这是 TcpServer 传给 Acceptor 的“指令”。
    // 当 Acceptor 拿到一个新连接的 fd 后，它不知道该怎么处理（它不管理业务线程池）。它通过这个回调，把 fd 扔回给 TcpServer，让TcpServer决定如何处理新连接
    NewConnectionCallback newConnectionCallback_;

    bool listenning_;

    /// 【高并发技巧】预留一个空闲的文件描述符，这是Muduo 网络库（以及 libev 等工业级库）中处理 FD 耗尽 (EMFILE 错误) 的经典做法
    /// 问题场景：
    /// 假设系统允许打开的最大 fd 数是 1000，现在已经用完了。此时有一个新连接到来，Listen Socket 变为可读（LT 模式）。
    /// 1. epoll_wait 返回，通知 Acceptor 去读。
    /// 2. Acceptor 调用 accept()。
    /// 3. accept() 返回 -1，errno 是 EMFILE（因为没额度给新连接分配 fd 了）。
    /// 4. 死循环开始：因为 accept 失败，连接没被取出来，它依然在内核的全连接队列里。下次 epoll_wait 会再次告诉你“有连接可读”。然后你又 accept 失败... 周而复始，CPU 直接 100%。
    ///
    /// 解决方案 (idleFd_)：
    /// 在构造函数里，先 open("/dev/null") 占住一个坑位（idleFd_）。
    /// 1. 当 accept 报错 EMFILE 时：
    /// 2. 关闭 idleFd_（腾出一个坑位）。
    /// 3. 再次 accept（此时会成功，拿到这个新连接的 clientfd）。
    /// 4. 立刻关闭 clientfd（优雅地踢掉这个用户，告诉他服务器忙）。
    /// 5. 重新 open("/dev/null") 把坑位占回来（为下一次 EMFILE 做准备）。
    /// 这个技巧保证了在 fd 耗尽时，服务器不会因为忙轮询而崩溃，而是能够优雅地拒绝新连接。
    int idleFd_;
};



#endif //ACCEPTOR_H
