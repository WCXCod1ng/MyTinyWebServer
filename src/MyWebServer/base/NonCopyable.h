//
// Created by user on 2025/11/25.
//

#ifndef NONCOPYABLE_H
#define NONCOPYABLE_H

class NonCopyable {
protected:
    NonCopyable() = default;
    ~NonCopyable() = default;

public:
    /// 通过删除拷贝构造函数和拷贝赋值函数，进制对象拷贝
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

#endif //NONCOPYABLE_H
