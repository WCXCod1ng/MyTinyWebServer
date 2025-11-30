//
// Created by user on 2025/11/25.
//

#include "Socket.h"
#include "InetAddress.h"
#include "log/logger.h"
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h> // for TCP_NODELAY

Socket::~Socket() {
    if (sockfd_ != -1) {
        ::close(sockfd_);
    }
}

void Socket::bindAddress(const InetAddress& localaddr) {
    if (0 != ::bind(sockfd_, (sockaddr*)localaddr.getSockAddr(), sizeof(sockaddr_in))) {
        LOG_ERROR("Socket::bindAddress error");
    }
}

void Socket::listen() {
    // SOMAXCONN 是系统允许的 backlog 最大值（半连接+全连接队列长度）
    if (0 != ::listen(sockfd_, SOMAXCONN)) {
        LOG_ERROR("Socket::listen error");
    }
}

int Socket::accept(InetAddress* peeraddr) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    std::memset(&addr, 0, sizeof(addr));

    // 这是一个阻塞调用，但我们的 Socket 在 Acceptor 初始化时会被设为 non-blocking
    // 或者我们将在 accept 之后设为 non-blocking
    // 此处调用 accept4 可以直接设置 SOCK_NONBLOCK | SOCK_CLOEXEC
    // 相比传统的 accept，accept4 多了一个 flag 参数。我们直接传入 SOCK_NONBLOCK | SOCK_CLOEXEC，省去了两次额外的 fcntl 系统调用，效率更高。这是 Linux 特有的 API（非 POSIX，但既然做 Linux Server 就可以用）
    int connfd = ::accept4(sockfd_, (sockaddr*)&addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (connfd >= 0) {
        peeraddr->setSockAddr(addr);
    } else {
        // 这里可能会发生错误，比如 EMFILE (fd 用完了)
        // 当然在ET模式下可能因为数据读完而导致connfd为-1，我们统一交给调用者处理
        LOG_ERROR("Socket::accept error");
    }
    return connfd;
}

void Socket::shutdownWrite() {
    if (::shutdown(sockfd_, SHUT_WR) < 0) {
        LOG_ERROR("Socket::shutdownWrite error");
    }
}

void Socket::setTcpNoDelay(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
}

void Socket::setReuseAddr(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

void Socket::setReusePort(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
}

void Socket::setKeepAlive(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
}