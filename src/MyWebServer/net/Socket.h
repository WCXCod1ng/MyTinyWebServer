//
// Created by user on 2025/11/25.
//

#ifndef SOCKET_H
#define SOCKET_H
#include "base/NonCopyable.h"

/// 职责：管理 fd 的创建与关闭（析构时自动 close），并提供 bind/listen/accept 等操作
class InetAddress;

class Socket : NonCopyable {
public:
    explicit Socket(int sockfd)
        : sockfd_(sockfd) {}

    ~Socket();

    int fd() const { return sockfd_; }

    void bindAddress(const InetAddress& localaddr);
    void listen();

    // accept 返回新连接的 fd
    // peeraddr 是输出参数，用于回传客户端的地址
    int accept(InetAddress* peeraddr);

    // 半关闭（只关闭写端），通常用于 HTTP/RPC 这种请求-响应模型
    void shutdownWrite();

    // --- Socket 选项设置 ---
    /// Nagle 算法为了减少网络拥塞，会把小的 TCP 包攒在一起发。但这会增加延迟。对于 WebServer 或 RPC 这种要求低延迟的场景，必须禁用它，让数据包“即发即走”。
    void setTcpNoDelay(bool on);
    /// 如果没有这个，服务器挂掉后（处于 TIME_WAIT 状态），必须等 2 分钟才能重启，这在生产环境是不可接受的
    void setReuseAddr(bool on);  // 允许重用本地地址
    /// 允许多个进程/线程绑定到同一个端口
    void setReusePort(bool on);
    void setKeepAlive(bool on);  // 开启 TCP 心跳保活

private:
    const int sockfd_;
};



#endif //SOCKET_H
