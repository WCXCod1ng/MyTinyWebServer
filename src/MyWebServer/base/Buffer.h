//
// Created by user on 2025/11/25.
//

#ifndef BUFFER_H
#define BUFFER_H

#include <vector>
#include <string>
#include <algorithm>
#include <cassert>

// Buffer 结构示意图:
// +-------------------+------------------+------------------+
// | prependable bytes |  readable bytes  |  writable bytes  |
// |                   |     (CONTENT)    |                  |
// +-------------------+------------------+------------------+
// |                   |                  |                  |
// 0    <=  p   < readIndex <= read < writeIndex <= write < size

class Buffer {
public:
    static constexpr size_t kCheapPrepend = 8;
    static constexpr size_t kInitialSize = 1024;

    explicit Buffer(const size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize),
          readIndex_(kCheapPrepend),
          writeIndex_(kCheapPrepend) {}

    /// 可写部分的字节数
    [[nodiscard]] size_t readableBytes() const { return writeIndex_ - readIndex_; }
    /// 可读部分的字节数
    [[nodiscard]] size_t writableBytes() const { return buffer_.size() - writeIndex_; }
    /// 最开始部分的字节数
    [[nodiscard]] size_t prependableBytes() const { return readIndex_; }

    /// 返回可读的第一个下标
    [[nodiscard]] const char* peek() const { return begin() + readIndex_; }

    /// 取出 len 字节的数据（移动 readIndex）
    void retrieve(const size_t len) {
        assert(len <= readableBytes());
        if (len < readableBytes()) {
            readIndex_ += len;
        } else {
            retrieveAll();
        }
    }

    /// 清空所有可读的字节数，将指针回到开始位置
    void retrieveAll() {
        readIndex_ = kCheapPrepend;
        writeIndex_ = kCheapPrepend;
    }

    /// 将所有数据转为 string 并清空 buffer
    std::string retrieveAllAsString() {
        return retrieveAsString(readableBytes());
    }

    /// 读取len个字符并以字符串形式返回
    std::string retrieveAsString(const size_t len) {
        assert(len <= readableBytes());
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }

    /// 写入数据
    void append(const std::string& str) { append(str.data(), str.size()); }
    void append(const char* data, const size_t len) {
        ensureWritableBytes(len);
        std::copy_n(data, len, beginWrite());
        writeIndex_ += len;
    }

    /// 写指针
    char* beginWrite() { return begin() + writeIndex_; }
    [[nodiscard]] const char* beginWrite() const { return begin() + writeIndex_; }

    /// 核心：从 fd 读取数据，并将读取到的数据写入该Buffer
    /// 返回读取的字节数，并保存 errno
    /// 支持分散读（scatter read）
    ssize_t readFd(int fd, int* saveErrno);
    /// 核心：写入 fd，将该Buffer中可读的部分写入socket缓冲区
    ssize_t writeFd(int fd, int* saveErrno);

private:
    char* begin() { return &*buffer_.begin(); }
    [[nodiscard]] const char* begin() const { return &*buffer_.begin(); }

    void ensureWritableBytes(const size_t len) {
        if (writableBytes() < len) {
            makeSpace(len);
        }
        assert(writableBytes() >= len);
    }

    // 扩容或整理空间
    void makeSpace(const size_t len) {
        // 如果 剩余可写空间 + 头部空闲空间 < len，则只能扩容
        if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
            buffer_.resize(writeIndex_ + len);
        } else {
            // 内部腾挪：把 readable 数据移到最前面
            const size_t readable = readableBytes();
            // copy允许源区间和目标区间重合（内部会使用一个缓冲区，所以不会因为数据重合而导致数据被覆盖）
            std::copy(begin() + readIndex_, begin() + writeIndex_, begin() + kCheapPrepend);
            readIndex_ = kCheapPrepend;
            writeIndex_ = readIndex_ + readable;
        }
    }

    std::vector<char> buffer_;
    size_t readIndex_;
    size_t writeIndex_;
};



#endif //BUFFER_H
