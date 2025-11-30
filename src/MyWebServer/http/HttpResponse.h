//
// Created by user on 2025/11/29.
//

#ifndef HTTPRESPONSE_H
#define HTTPRESPONSE_H
#include <string>
#include <unordered_map>

#include "base/Buffer.h"
#include "base/NonCopyable.h"

class Buffer;

// 常见状态码枚举
enum class HttpStatusCode {
    kUnknown,
    k200Ok = 200,
    k301MovedPermanently = 301,
    k400BadRequest = 400,
    k404NotFound = 404,
    k405MethodNotAllowed = 405,
    k500InternalServerError = 500,
};

class HttpResponse : NonCopyable {
public:
    explicit HttpResponse(bool close)
        : statusCode_(HttpStatusCode::kUnknown),
          closeConnection_(close) // 默认是否关闭由业务层决定
    {}

    // --- Setters ---

    void setStatusCode(const HttpStatusCode code) { statusCode_ = code; }
    void setStatusMessage(const std::string& message) { statusMessage_ = message; }
    void setCloseConnection(const bool on) { closeConnection_ = on; }

    bool closeConnection() const { return closeConnection_; }

    // 设置 Content-Type
    void setContentType(const std::string& contentType) {
        addHeader("Content-Type", contentType);
    }

    // 添加头部
    void addHeader(const std::string& key, const std::string& value) {
        headers_[key] = value;
    }

    // 设置响应体
    void setBody(const std::string& body) { body_ = body; }

    // --- 核心功能：序列化 ---

    // 将 HttpResponse 格式化写入 Buffer
    void appendToBuffer(Buffer* output) const {
        char buf[32];

        // 1. 响应行: HTTP/1.1 200 OK\r\n
        snprintf(buf, sizeof buf, "HTTP/1.1 %d ", static_cast<int>(statusCode_));
        output->append(buf);
        output->append(statusMessage_);
        output->append("\r\n");

        // 2. 响应头
        if (closeConnection_) {
            // 如果决定关闭连接，强制覆盖 Connection 头
            output->append("Connection: close\r\n");
        } else {
            // HTTP/1.1 默认 Keep-Alive，如果没设置 Content-Length，
            // 且没设置 close，需要确保 body 长度明确或使用 chunked（此处简化，假设有 body 就设 length）
            snprintf(buf, sizeof buf, "Content-Length: %zd\r\n", body_.size());
            output->append(buf);
            output->append("Connection: Keep-Alive\r\n");
        }

        // 添加其他头部
        for (const auto& header : headers_) {
            output->append(header.first);
            output->append(": ");
            output->append(header.second);
            output->append("\r\n");
        }

        // 头部结束空行
        output->append("\r\n");

        // 3. 响应体
        output->append(body_);
    }

private:
    /// 状态码
    HttpStatusCode statusCode_;
    /// 状态消息
    std::string statusMessage_;
    /// 是否需要关闭连接 (Connection: close)
    bool closeConnection_;

    /// 响应头
    std::unordered_map<std::string, std::string> headers_;
    /// 响应体
    std::string body_;
};

#endif //HTTPRESPONSE_H
