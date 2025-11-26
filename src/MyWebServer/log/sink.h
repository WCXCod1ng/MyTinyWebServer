//
// Created by user on 2025/11/18.
//

#ifndef SINK_H
#define SINK_H
#include <string>

class ISink {
public:
    virtual ~ISink() = default;

    // 纯虚函数，子类必须实现
    virtual void log(const std::string &formatted_message) = 0;

    // 纯虚函数，子类必须实现
    // 执行刷新操作
    virtual void flush() = 0;
};

#endif //SINK_H
