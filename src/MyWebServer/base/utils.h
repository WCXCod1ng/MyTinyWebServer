//
// Created by user on 2025/11/26.
//

#ifndef UTILS_H
#define UTILS_H
#include <pthread.h>
#include <string>

inline std::string getCurrentThreadName() {
    char name[16]{};
    // pthread_getname_np 完全支持 std::thread 创建的线程
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) == 0) {
        return std::string(name);
    }
    return "<unknown>";
}

inline void setCurrentThreadName(const std::string& name) {
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

#endif //UTILS_H
