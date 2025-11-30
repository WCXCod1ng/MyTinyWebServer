//
// Created by user on 2025/11/26.
//

#include "TcpServer.h"

#include "Acceptor.h"
#include "EventLoop.h"
#include "EventLoopThreadPool.h"
#include "TcpConnection.h"
#include "log/logger.h"

// 检查传入的 Loop 是否为空
static EventLoop* CheckLoopNotNull(EventLoop* loop) {
    if (loop == nullptr) {
        LOG_ERROR("TcpServer: loop is null");
        exit(1); // 极其严重的错误，直接退出
    }
    return loop;
}

/// 这里主EventLoop是由外部指针传入的，其实只要保证主EventLoop生命周期大于TcpServer即可。为什么使用指针而非引用，主要是遵循muduo库中“非拥有型关系用裸指针”的规范
/// 而且“一个主EventLoop会同时服务多个TcpServer”也造成了主EventLoop不能作为TcpServer的成员
TcpServer::TcpServer(EventLoop* loop,
                     const InetAddress& listenAddr,
                     const std::string& nameArg,
                     const Option option,
                     const size_t numThreads,
                     const double idleTimeoutSeconds)
    : baseLoop_(CheckLoopNotNull(loop)),
      ipPort_(listenAddr.toIpPort()),
      name_(nameArg),
      acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)),
      threadPool_(std::make_unique<EventLoopThreadPool>(loop, numThreads, name_)), // 使用TcpServer的名称作为线程池的名称（前缀）
      connectionCallback_(),
      messageCallback_(),
      nextConnId_(1),
      started_(0),
      idleTimeoutSeconds_(idleTimeoutSeconds)
{
    // 设置 Acceptor 的回调：当有新连接时，执行 TcpServer::newConnection
    acceptor_->setNewConnectionCallback(
        std::bind(&TcpServer::newConnection, this, std::placeholders::_1, std::placeholders::_2));
}

/// muduo网络库的析构顺序：
/// 1. TcpServer析构函数被触发，清理所有的TcpConnection（此时还计数为1，要等到connectDestroyed函数处理完后才会自动清零）
///     - 等待异步调用connectDestroyed()负责执行实际的removeChannel()操作，将Channel从Epoller中删除.
///     - connectDestroyed()结束后，TcpConnection因为引用计数减为0而调用~TcpConnection()：其内部使用unique_ptr管理了Channel，因此也触发了~Channel()（什么也不做，已经被connectDestroyed()清理完毕了）
/// 2. 由于TcpServer内部通过unique_ptr管理了EventLoopThreadPool，因此~TcpServer()被执行后，紧接着会析构EventLoopThreadPool，并简介析构EventLoopThread
/// 3. 主EventLoop的生命周期长于TcpServer，会在外部最后被析构
TcpServer::~TcpServer()
{
    baseLoop_->assertInLoopThread();
    LOG_INFO("TcpServer::~TcpServer [{}] destructing", name_);

    for (auto& item : connections_) {
        // 这个 conn 是局部临时智能指针，持有了连接对象
        // 这一步是为了让 map 中的引用计数减 1
        TcpConnectionPtr conn(item.second);
        item.second.reset(); // 释放 map 中保存的 shared_ptr，此时 conn 引用计数为 1

        // 销毁连接，必须在 IO 线程中做
        conn->getLoop()->runInLoop(
            std::bind(&TcpConnection::connectDestroyed, conn));
    }
}

void TcpServer::setThreadNum(const size_t numThreads)
{
    threadPool_->setThreadNum(numThreads);
}

void TcpServer::start()
{
    if (started_++ == 0) { // 防止被多次 start
        // 1. 启动线程池
        threadPool_->start(threadInitCallback_);

        // 2. 启动监听
        // 必须在 baseLoop 中运行 listen，否则并发会有问题
        baseLoop_->runInLoop(std::bind(&Acceptor::listen, acceptor_.get()));
    }
}

// [核心逻辑] 处理新连接，由监听socket触发读事件后回调
// 运行在 Main Loop (baseLoop)
void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr)
{
    baseLoop_->assertInLoopThread();

    // 1. 轮询选择一个 SubLoop (IO Loop)，新连接会在这个ioLoop中被处理（监听读、写事件，执行读、写操作）
    EventLoop* ioLoop = threadPool_->getNextLoop();

    // 2. 构建连接名称 (ServerName-IP:Port#ID)
    // char buf[64];
    // snprintf(buf, sizeof(buf), "-%s#%d", ipPort_.c_str(), nextConnId_);
    std::string buf = std::format("-{}#{}", ipPort(), nextConnId_);
    ++nextConnId_;
    std::string connName = name_ + buf;

    LOG_INFO("TcpServer::newConnection [{}] - new connection [{}] from {}", name_, connName, peerAddr.toIpPort());

    // 3. 获取本地地址 (getsockname)
    InetAddress localAddr(0); // 这里的实现略，通常可以通过 sockets::getLocalAddr(sockfd) 获取
    // 为了简化，这里先不实现 getLocalAddr，实际项目中建议补上

    // 4. 创建 TcpConnection 对象
    // 使用 shared_ptr 管理，引用计数初始化为 1
    TcpConnectionPtr conn(new TcpConnection(ioLoop,
                                            connName,
                                            sockfd,
                                            localAddr,
                                            peerAddr,
                                            60.0));

    // 5. 将连接放入 Map (Map 保存了 shared_ptr，引用计数 +1)
    connections_[connName] = conn;

    // 6. 设置用户回调
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    // 7. 设置关闭回调
    // 当 TcpConnection::handleClose 触发时，会回调 TcpServer::removeConnection
    conn->setCloseCallback(
        std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));

    // 8. 在 IO 线程中完成连接的最后初始化 (enableReading)
    ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
}

/// [核心逻辑] 移除连接
/// 当连接断开时，TcpConnection 会回调这里
/// 1. 触发：SubLoop 中，TcpConnection::handleRead 读到 0，调用 handleClose。
/// 2. 回调：handleClose 调用 closeCallback_，也就是 TcpServer::removeConnection。此时还在 SubLoop。
/// 3. 切换 1：TcpServer::removeConnection 调用 loop_->runInLoop(...)，切回 MainLoop 执行 removeConnectionInLoop。
///     - 原因：connections_ Map 属于 TcpServer，且非线程安全，必须在 MainLoop 操作。
/// 4. 执行：MainLoop 从 Map 中移除 conn。此时 conn 的引用计数减 1。
/// 5. 切换 2：MainLoop 调用 ioLoop->queueInLoop(...)，切回 SubLoop 执行 TcpConnection::connectDestroyed。
///     - 原因：connectDestroyed 要操作 Channel (remove from Poller)，而 Channel 属于 SubLoop，必须在 SubLoop 线程内操作，否则会有 Race Condition。
/// 6. 销毁：connectDestroyed 执行完毕，bind 绑定的 shared_ptr 释放，对象最终析构。
void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
    // 由于 removeConnection 可能会被多线程调用 (conn 在 subLoop 中)，
    // 我们必须保证从 map 中移除的操作发生在 mainLoop 中。
    baseLoop_->runInLoop(
        std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
    baseLoop_->assertInLoopThread();
    LOG_INFO("TcpServer::removeConnectionInLoop [{}] - connection {}", name_, conn->name());

    // 1. 从 Map 中删除，此时conn的引用计数减1
    size_t n = connections_.erase(conn->name());
    assert(n == 1);

    // 2. 销毁连接
    // 此时 conn 的生命周期由参数 conn (shared_ptr) 维持。
    // 我们需要在连接所属的 IO Loop 中执行 connectDestroyed。
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop(
        std::bind(&TcpConnection::connectDestroyed, conn));
}