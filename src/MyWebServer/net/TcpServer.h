//
// Created by user on 2025/11/26.
//

#ifndef TCPSERVER_H
#define TCPSERVER_H
#include <atomic>
#include <functional>
#include <map>

#include "Callbacks.h"
#include "InetAddress.h"
#include "base/NonCopyable.h"

class EventLoop;
class Acceptor;
class EventLoopThreadPool;

/**
 * @brief TCP 服务器
 *
 * 用户使用入口。
 *
 * 线程模型：
 * 1. Acceptor 运行在 baseLoop (主线程)
 * 2. 已连接的 TcpConnection 运行在 ioLoop (线程池中的子线程)
 * 3. 所有对 connections_ Map 的操作都在 baseLoop 中进行
 */
class TcpServer : NonCopyable {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    enum Option {
        kNoReusePort,
        kReusePort,
    };

    TcpServer(EventLoop* loop,
              const InetAddress& listenAddr,
              const std::string& nameArg,
              Option option = kNoReusePort,
              size_t numThreads = 0,
              double idleTimeoutSeconds = 60.0);
    ~TcpServer();

    // 设置线程数量 (0 表示单线程，>0 表示多线程 Reactor)
    void setThreadNum(size_t numThreads);

    /// 线程初始化回调 (在 start 之后，线程启动之前执行)
    void setThreadInitCallback(const ThreadInitCallback& cb) { threadInitCallback_ = cb; }

    // 开启服务器监听
    void start();

    // --- 注册用户回调 ---
    /// 设置连接建立或关闭时的回调
    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    /// 设置收到对端发送的消息后的回调
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    /// 设置写操作完毕后的回调
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }

    inline const std::string& ipPort() const { return ipPort_; }
    inline const std::string& name() const { return name_; }
    inline EventLoop* getLoop() const { return baseLoop_; }

    inline void setIdleTimeoutSeconds(const double seconds) {
        if(seconds > 0) {
            idleTimeoutSeconds_ = seconds;
        }
    }

private:
    /// Acceptor 有新连接时的回调
    void newConnection(int sockfd, const InetAddress& peerAddr);

    /// TcpConnection 断开时的回调
    void removeConnection(const TcpConnectionPtr& conn);

    /// 在 Loop 中移除连接的各种 helper
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

    using ConnectionMap = std::map<std::string, TcpConnectionPtr>;

    EventLoop* baseLoop_;  // baseLoop (用户定义的那个 loop)

    const std::string ipPort_;
    const std::string name_;

    // 核心组件
    /// 监听器，核心逻辑是accept一个新连接
    std::unique_ptr<Acceptor> acceptor_;
    /// 线程池，存储所有的IO线程（专用于处理已连接socket）
    std::unique_ptr<EventLoopThreadPool> threadPool_;

    // 回调函数
    ConnectionCallback connectionCallback_; // 连接建立/断开后的回调
    MessageCallback messageCallback_; // 收到对端发送的消息后的回调
    WriteCompleteCallback writeCompleteCallback_; // 写操作完毕后的回调
    ThreadInitCallback threadInitCallback_; // EventLoopThread的线程被创建（初始化）好后的回调

    std::atomic<int> started_;

    // 用于生成连接名称的 ID
    int nextConnId_;
    /// 为什么需要 connections_ 这个 Map？保存所有活跃连接 (Key: 连接名)
    /// TcpConnection 是通过 shared_ptr 管理的。当一个连接建立时，如果没有任何人持有它的 shared_ptr，出了 newConnection 作用域它就会被析构，连接就断了。
    /// 所以 TcpServer 用一个 Map 持有它，保证它活着。
    /// 当连接断开时，从 Map 移除，引用计数归零（假设用户回调不持有），对象析构
    ConnectionMap connections_;

    // 连接超时时间（用于移除长时间的连接）
    double idleTimeoutSeconds_;
};



#endif //TCPSERVER_H
