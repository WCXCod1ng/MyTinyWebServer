//
// Created by user on 2025/11/29.
//

#include "HttpContext.h"

#include "base/Buffer.h"

// static constexpr char kCLRF[] = "\r\n";

const std::string HttpContext::kCLRF = "\r\n";

// 核心解析函数
bool HttpContext::parseRequest(Buffer* buf, TimeStamp receiveTime) {
    bool ok = true;
    bool hasMore = true;

    while (hasMore) {
        // 1. 解析请求行 [GET /index.html HTTP/1.1]
        if (state_ == HttpRequestParseState::kExpectRequestLine) {
            const char* crlf_pos = buf->findStr(kCLRF);
            if (crlf_pos) {
                // 如果找到了 \r\n，说明有一行完整的数据
                // 处理请求行：从 buf->peek() 到 crlf
                ok = processRequestLine(buf->peek(), crlf_pos);
                if (ok) {
                    request_.setReceiveTime(receiveTime);
                    // 移动读指针：跳过请求行 + \r\n (2字节)
                    buf->retrieveUntil(crlf_pos + 2);
                    state_ = HttpRequestParseState::kExpectHeaders;
                } else {
                    hasMore = false; // 解析失败
                }
            } else {
                hasMore = false; // 数据不够，等待下一波数据
            }
        }
        // 2. 解析请求头 [Content-Length: 100]
        else if (state_ == HttpRequestParseState::kExpectHeaders) {
            const char* crlf_pos = buf->findStr(kCLRF);
            if (crlf_pos) {
                const char* colon = std::find(buf->peek(), crlf_pos, ':');
                if (colon != crlf_pos) {
                    // 这是一个普通头部字段
                    request_.addHeader(buf->peek(), colon, crlf_pos);
                } else {
                    // 没有冒号，说明这是空行 (\r\n)，头部结束！
                    // 状态流转：根据是否有 Content-Length 决定是否解析 Body
                    // 1. 检查是否有请求体
                    std::string lenStr = request_.getHeader("Content-Length");
                    if (!lenStr.empty()) {
                         // 有 body，转入 body 解析状态
                         state_ = HttpRequestParseState::kExpectBody;
                    } else {
                         // 没有 body (GET 请求通常如此)，直接完成
                         state_ = HttpRequestParseState::kGotAll;
                         hasMore = false; // 解析完成，跳出循环
                    }
                }
                // 移动 Buffer 指针
                buf->retrieveUntil(crlf_pos + 2);
            } else {
                hasMore = false; // 数据不够
            }
        }
        // 3. 解析请求体 (对应你的 ParseState::CONTENT)
        else if (state_ == HttpRequestParseState::kExpectBody) {
            // 获取 Content-Length
            size_t contentLength = 0;
            try {
                contentLength = std::stoll(request_.getHeader("Content-Length"));
            } catch (...) {
                // 如果长度解析失败，认为出错
                ok = false;
                hasMore = false;
                break;
            }

            // 检查 Buffer 里的数据够不够
            if (buf->readableBytes() >= contentLength) {
                // 数据足够！读取 body
                request_.setBody(buf->peek(), buf->peek() + contentLength);

                // 移动 Buffer 指针，吃掉 body
                buf->retrieve(contentLength);

                state_ = HttpRequestParseState::kGotAll;
                hasMore = false; // 完成
            } else {
                // 数据不够，等待 TCP 分包到达
                hasMore = false;
            }
        }
    }

    return ok;
}

/// 解析请求行，GET /index.html?name=tom HTTP/1.1
bool HttpContext::processRequestLine(const char* begin, const char* end) {
    bool succeed = false;
    const char* start = begin;
    // 查找第一个空格 (Method 结束)
    const char* space_pos = std::find(start, end, ' ');

    if (space_pos != end && request_.setMethod(start, space_pos)) {
        start = space_pos + 1;
        // 查找第二个空格 (Path 结束)
        space_pos = std::find(start, end, ' ');
        if (space_pos != end) {
            // 解析 Path 和 Query
            const char* question = std::find(start, space_pos, '?');
            if (question != space_pos) {
                request_.setUrl(start, question);
                request_.setQueries(question+1, space_pos); // name -> tom
            } else {
                request_.setUrl(start, space_pos);
            }
            // request_.setUrl(start, space_pos); // “/index.html?name=tom”

            // 解析 Version
            start = space_pos + 1;
            std::string version(start, end);
            if (version == "HTTP/1.1") {
                request_.setVersion(Version::kHttp11);
                succeed = true;
            } else if (version == "HTTP/1.0") {
                request_.setVersion(Version::kHttp10);
                succeed = true;
            } // 其他情况出错
        }
    }
    return succeed;
}