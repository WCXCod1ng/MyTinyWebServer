//
// Created by user on 2025/11/26.
//

#include "TcpConnection.h"

#include "Channel.h"
#include "EventLoop.h"
#include "Socket.h"
#include "log/logger.h"

// 静态成员初始化
void defaultConnectionCallback(const TcpConnectionPtr& conn) {
    LOG_INFO("Connection {} is {}", conn->name(), (conn->connected() ? "UP" : "DOWN"));
}

void defaultMessageCallback(const TcpConnectionPtr&, Buffer* buf, TimeStamp) {
    buf->retrieveAll(); // 默认丢弃所有数据，防止堆积
}

TcpConnection::TcpConnection(EventLoop* loop,
                             const std::string& nameArg,
                             const int sockfd,
                             const InetAddress& localAddr,
                             const InetAddress& peerAddr,
                             const double idleTimeoutSeconds)
    : ioLoop_(loop),
      name_(nameArg),
      state_(kConnecting),
      reading_(true),
      socket_(new Socket(sockfd)),
      channel_(new Channel(loop, sockfd)),
      localAddr_(localAddr),
      peerAddr_(peerAddr),
      highWaterMark_(64 * 1024 * 1024), // 64MB
      idleTimeoutSeconds_(idleTimeoutSeconds)
{
    // 给 Channel 设置回调函数
    // 当 Poller 监听到事件后，会调用 Channel::handleEvent，进而调用这些函数
    channel_->setReadCallback(
        std::bind(&TcpConnection::handleChannelRead, this, std::placeholders::_1));
    channel_->setWriteCallback(
        std::bind(&TcpConnection::handleChannelWrite, this));
    channel_->setCloseCallback(
        std::bind(&TcpConnection::handleChannelClose, this));
    channel_->setErrorCallback(
        std::bind(&TcpConnection::handleChannelError, this));

    LOG_INFO("TcpConnection::ctor[{}] at fd={}", name_, sockfd);
    socket_->setKeepAlive(true); // 开启 TCP 保活
}

TcpConnection::~TcpConnection()
{
    LOG_INFO("TcpConnection::dtor[{}] at fd={} state={}", name_, channel_->fd(), static_cast<int>(state_));
}

// [核心] 连接建立完成
// 此函数由 TcpServer::newConnection 调用（在 MainLoop 中创建对象后，在 SubLoop 中执行此函数）
// 它是连接生命的起点。虽然构造函数创建了对象，但直到这个函数被调用，连接才真正开始监听读事件（enableReading）
void TcpConnection::connectEstablished()
{
    ioLoop_->assertInLoopThread();
    assert(state_ == kConnecting);
    setState(kConnected);

    // 【重要】将 TcpConnection (shared_ptr) 绑定到 Channel 的 shared_ptr
    // 防止 Channel 在执行回调时，TcpConnection 对象已经被销毁
    channel_->tie(shared_from_this());

    // 向 Poller 注册 读 事件
    // 这是生命周期管理最关键的一步。如果不绑定，当连接断开时，TcpConnection 可能在 handleClose 执行一半时就被 TcpServer 从 map 中移除并析构，导致后续代码访问非法内存。
    channel_->enableReading();

    // 回调用户注册的 ConnectionCallback (告诉业务层：连接成功了)
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }

    // 连接建立时启动定时器
    extendLifetime();
}

/// 已连接Channel发现可读时实际上会调用该函数，执行实际的读事件
void TcpConnection::handleChannelRead(TimeStamp receiveTime)
{
    ioLoop_->assertInLoopThread();
    int savedErrno = 0;

    // 有数据来时说明对端还存活，应当刷新定时器
    extendLifetime();

    // 从 Socket 读取数据读到 inputBuffer_
    // 利用 Buffer::readFd 的 readv 技术，自动处理读不够的情况
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);

    if (n > 0) {
        // 读取成功，回调用户的 onMessage，表示收到消息
        if (messageCallback_) {
            messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
        }
    } else if (n == 0) {
        // 读到 0 字节，表示对端关闭了连接 (FIN)
        LOG_WARN("对端关闭，调用TcpConnection::handleClose()");
        handleChannelClose();
    } else {
        // 出错
        errno = savedErrno;
        LOG_ERROR("TcpConnection::handleRead");
        handleChannelError();
    }
}

// [核心] 发送数据的对外接口
void TcpConnection::send(const std::string& buf)
{
    if (state_ == kConnected) {
        if (ioLoop_->isInLoopThread()) {
            // 在自己线程中调用，则直接执行
            sendInLoop(buf.c_str(), buf.size());
        } else {
            // 如果在其他线程调用 send，需要转发给 IO 线程
            // 注意：这里必须拷贝 buf，因为它是引用，跨线程可能会失效
            // 优化点：C++11 move 语义可以减少拷贝，或者 sendInLoopString
            void (TcpConnection::*fp)(const void* data, size_t len) = &TcpConnection::sendInLoop;
            ioLoop_->runInLoop(std::bind(fp, this, buf.c_str(), buf.size()));
        }
    }
}

void TcpConnection::send(const void *message, size_t len) {
    if (state_ == kConnected) {
        if (ioLoop_->isInLoopThread()) {
            // 在自己线程中调用，则直接执行
            sendInLoop(message, len);
        } else {
            // 如果在其他线程调用 send，需要转发给 IO 线程
            // 注意：这里必须拷贝 buf，因为它是引用，跨线程可能会失效
            // 优化点：C++11 move 语义可以减少拷贝，或者 sendInLoopString
            void (TcpConnection::*fp)(const void* data, size_t len) = &TcpConnection::sendInLoop;
            ioLoop_->runInLoop(std::bind(fp, this, message, len));
        }
    }
}

// [核心] IO 线程内真正的发送逻辑
void TcpConnection::sendInLoop(const void* data, size_t len)
{
    ioLoop_->assertInLoopThread();
    ssize_t nWritten = 0;
    size_t remaining = len;
    bool faultError = false;

    // 之前已经调用过 shutdown，不能再发送了
    if (state_ == kDisconnected) {
        LOG_ERROR("disconnected, give up writing");
        return;
    }

    // 1. 尝试直接写 (Zero Copy 优化)
    // 条件：outputBuffer 没有积压数据（保证数据发送的有序性、outputBuffer中的数据是先发送的，应当先被write进socket） && Channel 没有关注写事件（关注了写事件会怎样？）
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        LOG_DEBUG("触发zero copy优化，直接将待发送的数据写入socket");

        // fixme 直接写入时是否需要延长？
        extendLifetime();

        const char* ptr = static_cast<const char*>(data);
        // note ET模式下循环写，直到写完（或者写满）
        while(remaining > 0) {
            ssize_t n = ::write(channel_->fd(), data, len);
            if(n > 0) {
                // 写入成功，更新数据
                nWritten += n;
                remaining -= n;
                ptr += n;
            } else {
                if(errno == EINTR) continue;
                // ET模式写的结束（内核缓冲区已满），剩下的数据必须放到outputBuffer_中（由“2”负责）
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                // 真正的错误
                LOG_ERROR("直接写操作出错");
                nWritten = 0;
                if (errno != EWOULDBLOCK) {
                    if (errno == EPIPE || errno == ECONNRESET) {
                        faultError = true;
                    }
                }
            }
        }

        // 如果没有出错的写完，则触发写完的回调
        if(!faultError && remaining == 0 && writeCompleteCallback_) {
            ioLoop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
        }
    }

    // 2. 如果没有写完（Socket 缓冲区满了）或者之前就有积压
    // 把剩余数据追加到 outputBuffer_，并开始关注 EPOLLOUT
    if (!faultError && remaining > 0) {
        size_t oldLen = outputBuffer_.readableBytes();

        // 高水位回调判断
        // 防止发送速度远快于对端接收速度，导致服务器内存耗尽
        if (oldLen + remaining >= highWaterMark_
            && oldLen < highWaterMark_
            && highWaterMarkCallback_)
        {
            ioLoop_->queueInLoop(std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
        }

        // 把剩余的部分追加到outputBuffer_中
        outputBuffer_.append(static_cast<const char*>(data) + nWritten, remaining);

        if (!channel_->isWriting()) {
            channel_->enableWriting(); // 注册写事件，等待 Epoll 通知 handleWrite，注意确保内部增加了EPOLLET
        }
    }
}

void TcpConnection::sendInLoopString(const std::string &message) {
    throw std::runtime_error("未实现");
}

/// 已连接Channel发现可写时实际上会调用该函数，执行实际的写事件
/// 从Buffer中的“可读区”读取数据并写入socket
void TcpConnection::handleChannelWrite()
{
    ioLoop_->assertInLoopThread();
    if (channel_->isWriting()) { // 当前Channel关注了写事件

        // 有数据可写入时，可以刷新定时器
        extendLifetime();

        int saveErrno = 0;
        // 将 outputBuffer_ 中的数据写入 Socket
        ssize_t n = outputBuffer_.writeFd(channel_->fd(), &saveErrno);

        if (n >= 0) {
            // 检查数据是否发送完
            if (outputBuffer_.readableBytes() == 0) {

                // 取消关注写事件，避免 busy loop，另外也是表明数据已经被发送完毕，如果现在要关闭，可以执行shutdownInLoop了
                // 而且也会导致ET/LT模式空转
                channel_->disableWriting();

                // 触发 WriteComplete 回调
                if (writeCompleteCallback_) {
                    ioLoop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
                }

                // 如果用户之前调用了 shutdown，但因为有数据没发完而被推迟了
                // 现在发完了，执行真正的 shutdown
                if (state_ == kDisconnecting) {
                    shutdownInLoop();
                }
            } else {
                // Buffer还没空，说明遇到EAGAIN了
                // 在ET模式下继续保持关注EPOLLOUT
                // note 只有使用了 EPOLLONESHOT 标志时，才需要在每次事件触发后重新注册（Rearm）。但在标准的 Reactor 模型（包括 Muduo 和你的实现）中，我们通常不使用 ONESHOT，所以不需要重复注册
                LOG_INFO("ET write EAGAIN, wait for next EPOLLOUT");
            }
        } else {
            LOG_ERROR("TcpConnection::handleWrite");
        }
    } else {
        LOG_INFO("Connection fd={} is down, no more writing", channel_->fd());
    }
}

// [主动关闭]
// 只是设置状态为正在关闭
void TcpConnection::shutdown()
{
    if (state_ == kConnected) {
        setState(kDisconnecting);
        ioLoop_->runInLoop(std::bind(&TcpConnection::shutdownInLoop, this));
    }
}

// 真正的关闭操作（socket_->shutdownWrite()）必须等到缓冲区数据清空后才能执行。这就是为什么在 handleWrite 里要检查 kDisconnecting 状态
void TcpConnection::shutdownInLoop()
{
    ioLoop_->assertInLoopThread();
    // 只有当 outputBuffer 数据全部发完，才能关闭写端
    // outputBuffer数据没有发送完，channel就还继续监听写事件
    if (!channel_->isWriting()) {
        socket_->shutdownWrite(); // 关闭写端，发送 FIN
    }
    // 如果 isWriting() 为真，说明还有数据没发完。
    // 我们只需设置 state_ = kDisconnecting。
    // 等 handleWrite 把数据发完后，会检查这个状态并执行 shutdownInLoop。
}

// [主动关闭] 强制关闭
void TcpConnection::forceClose()
{
    if (state_ == kConnected || state_ == kDisconnecting) {
        setState(kDisconnecting);
        ioLoop_->queueInLoop(std::bind(&TcpConnection::forceCloseInLoop, this));
    }
}

void TcpConnection::forceCloseInLoop()
{
    ioLoop_->assertInLoopThread();
    if (state_ == kConnected || state_ == kDisconnecting) {
        handleChannelClose();
    }
}

void TcpConnection::extendLifetime() {
    // 如果定时器句柄是dangling的，说明这是首次调用extendLifetime（也即TcpConnection建立后通过connectEstablished所调用的），那么不需要操作，直接等待赋值即可
    // 如果不是dangling的，那么先取消旧的定时器
    if(ioLoop_ && !idleTimer_.dangling()) {
        ioLoop_->cancel(idleTimer_);
    }

    // 添加新的
    idleTimer_ = ioLoop_->runAfter(idleTimeoutSeconds_, std::bind(&TcpConnection::handleTimeout, shared_from_this()));
}

void TcpConnection::handleTimeout() {
    // 定时器到期，说明这段时间内没有 extendLife 被调用
    LOG_INFO("TcpConnection::handleTimeout - Force Close fd={}", channel_->fd());

    // 强制关闭连接
    forceClose();
    // forceClose 会触发 handleClose -> removeConnection -> connectDestroyed
    // 最终完成清理
}

/// [被动关闭] 已连接Channel发现连接完全关闭时实际上会调用该函数，执行实际的关闭事件
/// 是触发动作。当 read 返回 0 或 forceClose 被调用时执行。它的核心任务是通知 TcpServer：“我这个连接完了，请把我删掉”
void TcpConnection::handleChannelClose()
{
    ioLoop_->assertInLoopThread();
    LOG_INFO("对端连接关闭：fd={} state={}", channel_->fd(), (int)state_);
    setState(kDisconnected);

    // 1. 停止关注所有事件
    channel_->disableAll();

    // 2. 这里的 connectionCallback_ 依然是用户的 (通知用户连接断开了)
    TcpConnectionPtr guardThis(shared_from_this());
    if (connectionCallback_) {
        connectionCallback_(guardThis);
    }

    // 3. 这里的 closeCallback_ 是 TcpServer 注册的 (TcpServer::removeConnection)
    // 也就是通知 TcpServer 把这个 conn 从 map 中移除
    if (closeCallback_) {
        closeCallback_(guardThis);
    }
}

/// 已连接Channel发现连接出异常错误时实际上会调用该函数，执行实际的异常处理事件
void TcpConnection::handleChannelError()
{
    int optval;
    socklen_t optlen = sizeof optval;
    int err = 0;
    // 获取具体的 socket 错误码
    if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
        err = errno;
    } else {
        err = optval;
    }
    LOG_ERROR("TcpConnection::handleError name:{} - SO_ERROR:{}", name_, err);

    // 出错后通常连接也不可用了，直接处理关闭
    // handleClose();
}

// [最后一步] 连接彻底销毁
// 由 TcpServer 从 map 中移除 conn 后调用
// connectDestroyed: 是清理动作。TcpServer 在删除 shared_ptr 之前，最后调用一次这个函数，确保 Channel 从 Epoll 中移除
void TcpConnection::connectDestroyed()
{
    ioLoop_->assertInLoopThread();

    if (state_ == kConnected) {
        setState(kDisconnected);
        channel_->disableAll();
        if (connectionCallback_) {
            connectionCallback_(shared_from_this());
        }
    }

    // 连接关闭时应取消定时器
    // 如果不取消，TimerQueue中的回调函数（bind）将持有conn的shared_ptr，导致conn无法析构
    if(ioLoop_) {
        ioLoop_->cancel(idleTimer_);
    }

    // 从 Poller 中彻底移除 Channel
    channel_->remove();
}