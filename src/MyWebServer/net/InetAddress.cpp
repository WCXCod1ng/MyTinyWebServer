//
// Created by user on 2025/11/25.
//

#include "InetAddress.h"

// src/net/InetAddress.cpp
#include "InetAddress.h"
#include <cstring>

InetAddress::InetAddress(uint16_t port, std::string ip) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    // inet_addr 已经过时，推荐使用 inet_pton
    if(::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) <= 0) {
        // 出错处理，通常记录日志或抛异常，这里为了简单默认 INADDR_ANY
        // addr_.sin_addr.s_addr = INADDR_ANY;
    }
}

std::string InetAddress::toIp() const {
    char buf[64] = {0};
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
    return buf;
}

std::string InetAddress::toIpPort() const {
    char buf[64] = {0};
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
    size_t end = std::strlen(buf);
    uint16_t port = ntohs(addr_.sin_port);
    sprintf(buf + end, ":%u", port);
    return buf;
}

uint16_t InetAddress::toPort() const {
    return ntohs(addr_.sin_port);
}