//
// Created by user on 2025/11/25.
//

#include "Buffer.h"
#include <sys/uio.h> // for readv
#include <unistd.h>  // for write
#include <cerrno>

#include "log/logger.h"

ssize_t Buffer::readFd(int fd, int* saveErrno) {
    // 使用栈上的临时空间，避免一开始就分配巨大的 vector
    char extrabuf[65536]; // 64k
    struct iovec vec[2] = {};

    ssize_t n = 0; // 本次读取的字节数
    ssize_t totalLen = 0; // 总共读取的字节数

    // note ET模式的核心：循环读取
    while(true) {
        vec[0].iov_base = begin() + writeIndex_;
        vec[0].iov_len = writableBytes();
        vec[1].iov_base = extrabuf;
        vec[1].iov_len = sizeof(extrabuf);

        const int iovcnt = (writableBytes() < sizeof(extrabuf)) ? 2 : 1;

        n = ::readv(fd, vec, iovcnt);

        // 检查本次读取的结果
        if (n < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // ET模式结束的标志：数据读取完毕
                *saveErrno = errno;
                break;
            }
            // 其他情况表示出错
            *saveErrno = errno;
            return -1;
        } else if(n == 0) {
            // 如果为0表示对端关闭
            // 这里通过返回0表示对端关闭
            *saveErrno = 0;
            if(totalLen == 0) {
                return 0;
            }
            break;
        } else {
            if (static_cast<size_t>(n) <= writableBytes()) {
                // 还没填满 vector
                writeIndex_ += n;
            } else {
                // 填满了 vector，剩下的在 extrabuf 里
                size_t old_writables = writableBytes();
                writeIndex_ = buffer_.size(); // 注意，要更新写指针，这样下一次readv的参数才正确
                append(extrabuf, n - old_writables); // 注意，不能直接使用，因为writeIndex_赋值时writableBytes()将变为0
            }

            // 累加totalLen的结果
            totalLen += n;
        }

    }

    return totalLen;
}

ssize_t Buffer::writeFd(int fd, int* saveErrno) {
    LOG_DEBUG("begin write");
    ssize_t totalWritten = 0;
    // note ET模式的核心：循环写入
    while(readableBytes() > 0) {
        ssize_t nWritten = ::write(fd, peek(), readableBytes());

        if(nWritten > 0) {
            // 更新索引
            retrieve(nWritten);
            totalWritten += nWritten;

            // 如果buffer为空，直接跳出
            if(readableBytes() == 0) break;
        } else if(nWritten <= 0) {
            if(errno == EINTR) {
                continue; // 信号中断，重试
            }
            // ET结束的标志
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            // 真正的错误
            *saveErrno = errno;
            return -1;
        }
    }
    LOG_DEBUG("write return with {} written", totalWritten);
    // 返回实际写入的数据
    return totalWritten;
}