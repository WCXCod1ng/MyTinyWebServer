//
// Created by user on 2025/11/26.
//

#ifndef UTILS_H
#define UTILS_H
#include <pthread.h>
#include <string>
#include <optional>
#include <sstream>

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

inline std::optional<unsigned char> hex_char_to_val(const char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return std::nullopt;
}

/// 用于解码url的工具函数
inline std::string url_decode(const std::string_view encoded_str, bool plus_to_space = false) {
    std::stringstream decoded_stream;

    for (size_t i = 0; i < encoded_str.length(); ++i) {
        if (encoded_str[i] == '%' && i + 2 < encoded_str.length()) {
            // 确保后面有两个字符
            auto high = hex_char_to_val(encoded_str[i + 1]);
            auto low = hex_char_to_val(encoded_str[i + 2]);

            if (high && low) {
                // 如果两个十六进制字符都有效
                unsigned char byte = (*high << 4) | *low;
                decoded_stream << static_cast<char>(byte);
                i += 2; // 跳过已经处理的两个十六进制字符
            } else {
                // 无效的十六进制，按原样添加 '%'
                decoded_stream << encoded_str[i];
            }
        } else if (plus_to_space && encoded_str[i] == '+') {
            decoded_stream << ' ';
        } else {
            decoded_stream << encoded_str[i];
        }
    }
    return decoded_stream.str();
}

#endif //UTILS_H
