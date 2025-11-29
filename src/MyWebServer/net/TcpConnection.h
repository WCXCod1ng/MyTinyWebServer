//
// Created by user on 2025/11/26.
//

#ifndef TCPCONNECTION_H
#define TCPCONNECTION_H
#include <any>
#include <atomic>
#include <memory>

#include "Callbacks.h"
#include "InetAddress.h"
#include "TimerId.h"
#include "base/Buffer.h"
#include "base/NonCopyable.h"
#include "base/TimeStamp.h"


class Channel;
class EventLoop;
class Socket;

/// 在 Reactor 模型中，TcpConnection 的设计原则是：一切皆异步，一切皆回调
/// 使用了 std::enable_shared_from_this。这是现代 C++ 网络编程的标配，用于保证在异步回调执行时，TcpConnection 对象本身不会被析构
/**
 * @brief TCP 连接封装
 *
 * 职责：
 * 1. 管理一个已连接 Socket 的生命周期
 * 2. 封装数据的收发（Non-blocking IO）
 * 3. 对外提供发送数据接口 (send)
 * 4. 处理连接的断开 (shutdown/close)
 *
 * 继承 enable_shared_from_this 的原因：
 * 我们需要在回调函数中（比如 runInLoop）传递当前对象的 shared_ptr，
 * 保证在回调执行期间对象不被销毁。
 */
class TcpConnection : NonCopyable, public std::enable_shared_from_this<TcpConnection> {
public:
    TcpConnection(EventLoop* loop,
                  const std::string& name,
                  int sockfd,
                  const InetAddress& localAddr,
                  const InetAddress& peerAddr,
                  const double idleTimeoutSeconds);
    ~TcpConnection();

    EventLoop* getLoop() const { return ioLoop_; }
    const std::string& name() const { return name_; }
    const InetAddress& localAddress() const { return localAddr_; }
    const InetAddress& peerAddress() const { return peerAddr_; }

    bool connected() const { return state_ == kConnected; }

    // --- 数据发送接口 ---
    // 线程安全，可以跨线程调用
    void send(const std::string& buf);
    void send(const void* message, size_t len);

    // --- 连接控制接口 ---
    // 关闭连接（优雅关闭：发送完缓冲区数据后关闭写端）
    void shutdown();

    // 强制关闭
    void forceClose();

    // --- 回调注册接口 ---
    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }
    void setCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }
    void setHighWaterMarkCallback(const HighWaterMarkCallback& cb, size_t highWaterMark) {
        highWaterMarkCallback_ = cb; highWaterMark_ = highWaterMark;
    }

    // --- 内部生命周期管理 (由 TcpServer 调用) ---
    // 连接建立时的 hook
    void connectEstablished();
    // 连接销毁时的 hook
    void connectDestroyed();

    // --- 上下文管理 (用于 HTTP 解析) ---
    void setContext(const std::any& context) { context_ = context; }
    const std::any& getContext() const { return context_; }
    std::any* getMutableContext() { return &context_; }

private:
    enum StateE {
        kDisconnected, // 关闭状态
        kConnecting, // 正在打开（连接）状态
        kConnected, // 已连接状态
        kDisconnecting // 正在关闭
    };

    // --- 为Channel准备的事件回调函数组---
    /// Channel中可读事件触发时执行的回调
    void handleChannelRead(TimeStamp receiveTime);
    void handleChannelWrite();
    void handleChannelClose();
    void handleChannelError();

    // --- 跨线程辅助函数 ---
    void sendInLoop(const void* message, size_t len);
    void sendInLoopString(const std::string& message); // 避免拷贝
    void shutdownInLoop();
    void forceCloseInLoop();

    void setState(const StateE s) { state_ = s; }

    // --- 定时器相关 ---
    /// 刷新定时器的生存期
    void extendLifetime();

    /// 定时器到期后的回调（准确来讲是timerfd到期后会向触发一个读事件，与之相关的Channel的回调就会被调用，并且又会调用定时器的回调，将来handleTimeout就是作为定时器的回调的）
    void handleTimeout();


    /// 所属的 SubReactor
    /// 一个EventLoop在运行期间可以管理成千上万的TcpConnection，但一个TcpConnection在其整个生命周期内只属于一个EventLoop，该TcpConnection归属于哪个EventLoop，是咋accept之后就确定好的了（之后永远不变）
    /// note 这里不能替换为引用，因为在极端情况下TcpConnection的生命周期比EventLoop还要长，这实际上是逻辑错误（编译器会假设引用在其生命周期内一直有效，可能会导致错误的优化）；而使用指针时，只是野指针，编译器允许这种行为
    /// 因为TcpConnection是shared_ptr，而它所属的EventLoop只是一个线程栈上的对象，在该EventLoop析构之后，该TcpConnection可能会因为回调而被其他线程持有，这样就造成了TcpConnection生命周期长于所属的EventLoop的现象
    EventLoop* const ioLoop_;
    // 名称
    const std::string name_;

    /// 状态机
    std::atomic<StateE> state_;
    bool reading_;

    /// 该TCP连接对应的socket
    std::unique_ptr<Socket> socket_;
    /// 该TCP连接对应的socket的Channel（管理事件）
    std::unique_ptr<Channel> channel_;

    // 地址信息
    const InetAddress localAddr_; // 本地地址
    const InetAddress peerAddr_; // 对端地址（客户端地址）

    // 回调函数
    ConnectionCallback connectionCallback_;       // 连接建立/断开回调
    MessageCallback messageCallback_;             // 收到消息回调
    WriteCompleteCallback writeCompleteCallback_; // 消息发送完毕回调
    HighWaterMarkCallback highWaterMarkCallback_; // 高水位回调
    CloseCallback closeCallback_;                 // 内部关闭（被动）回调（通知 TcpServer 移除自己）

    // 缓冲区
    size_t highWaterMark_; // 高水位阈值 (默认 64MB)
    Buffer inputBuffer_;   // 接收缓冲区
    Buffer outputBuffer_;  // 发送缓冲区

    // 定时器相关
    // 如果大于0表示允许keepAlive倒计时，也即对于keepAlive，在倒计时结束后自动断开连接，单位是秒
    // 如果等于0表示用不超时
    double idleTimeoutSeconds_;
    TimerId idleTimer_;

    // 任意类型的上下文 (存储 HttpContext)
    std::any context_;
};



#endif //TCPCONNECTION_H
