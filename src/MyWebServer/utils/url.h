//
// Created by user on 2025/11/17.
//

#ifndef URL_H
#define URL_H
#include <optional>
#include <string>
#include <sstream>

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

// 主解码函数
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


#endif //URL_H
