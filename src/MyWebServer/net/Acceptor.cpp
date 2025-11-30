//
// Created by user on 2025/11/25.
//

#include "Acceptor.h"
#include "Acceptor.h"
#include "log/logger.h"
#include "InetAddress.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

#include "EventLoop.h"

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport)
    : loop_(loop),
      acceptSocket_(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP)), // 注意设置监听socket为非阻塞
      acceptChannel_(loop, acceptSocket_.fd()), // 绑定 Channel 与 listenfd
      listenning_(false),
      idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)) // 预先占位
{
    // 1. 设置地址重用 (必须设置，否则重启服务会报错 "Address already in use")
    acceptSocket_.setReuseAddr(true);

    // 2. 设置端口重用 (Linux Kernel 3.9+ 支持，允许高并发下的多线程侦听同一个端口)
    acceptSocket_.setReusePort(reuseport);

    // 3. 绑定地址
    acceptSocket_.bindAddress(listenAddr);

    // 4. 注册 Channel 的读回调
    // 当有新连接到来时，执行 Acceptor::handleRead
    TimeStamp time_stamp = TimeStamp::now();
    acceptChannel_.setReadCallback(std::bind(&Acceptor::handleRead, this));

    LOG_INFO("Acceptor create non-blocking socket, fd={}", acceptChannel_.fd());
}

Acceptor::~Acceptor()
{
    acceptChannel_.disableAll(); // 停止关注事件
    acceptChannel_.remove();     // 从 Poller 中移除
    ::close(idleFd_);            // 关闭占位 fd
}

void Acceptor::listen()
{
    listenning_ = true;
    acceptSocket_.listen(); // 开启 listen 系统调用

    // 注册 EPOLLIN 事件到 Poller
    acceptChannel_.enableReading();

    LOG_INFO("Acceptor is listening");
}

// 监听socket中触发了读事件后的回调函数：需要处理新连接
void Acceptor::handleRead()
{
    // 此时运行在 Main Loop 线程
    loop_->assertInLoopThread();

    LOG_INFO("有新连接到来");

    InetAddress peerAddr;

    // ET模式的核心：循环Accept
    while(true) {
        // 1. 获取新连接
        int connfd = acceptSocket_.accept(&peerAddr);

        if (connfd >= 0) {
            // success
            if (newConnectionCallback_) {
                // 将新连接分发给 TcpServer
                // TcpServer 随后会创建一个 TcpConnection 并绑定到一个 IOLoop
                newConnectionCallback_(connfd, peerAddr);
            } else {
                // 如果没有设置回调，直接关闭，防止资源泄漏
                ::close(connfd);
            }
        } else {
            // ET模式的结束
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            // 出现真正的错误
            LOG_ERROR("Acceptor::handleRead accept error, errno={}", errno);

            // 错误描述：Process open FD exceeded (进程打开的文件描述符达到上限)
            // 核心在于使用一个idleFd_来软着陆
            if (errno == EMFILE) {
                // 1. 先关闭预留的空闲 fd，腾出一个坑位
                ::close(idleFd_);

                // 2. 再次 accept，此时因为有一个空坑位，accept 应该会成功
                // 这一步是为了把这个连接从内核的“全连接队列”中取出来
                // 否则 Poller (LT模式) 会一直触发 EPOLLIN，导致 busy loop (CPU 100%)
                idleFd_ = ::accept(acceptSocket_.fd(), nullptr, nullptr);

                // 3. 接受上来后，立刻关闭它（优雅地拒绝客户端）
                ::close(idleFd_);

                // 4. 重新把这个坑位占上，为下一次故障做准备
                idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);

                // 注意：处理完 EMFILE 后最好 break，等待下一次触发
                break;
            }

            // 其他错误页需要break，防止死循环
            break;
        }
    }
}