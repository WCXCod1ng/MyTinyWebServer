//
// Created by user on 2025/11/29.
//

#ifndef HTTPCONTEXT_H
#define HTTPCONTEXT_H
#include "HttpRequest.h"

class TimeStamp;
class Buffer;

class HttpContext {
public:
    enum class HttpRequestParseState {
        kExpectRequestLine, // 解析请求行
        kExpectHeaders,     // 解析请求头
        kExpectBody,        // 解析请求体
        kGotAll,            // 解析完毕
    };

    static const std::string kCLRF;

    HttpContext()
        : state_(HttpRequestParseState::kExpectRequestLine)
    {}


    // 核心入口：解析 Buffer 中的请求
    // 返回值：true 表示成功解析出一个完整的请求；false 表示数据不够或解析失败
    bool parseRequest(Buffer* buf, TimeStamp receiveTime);

    // 判断是否解析完毕
    bool gotAll() const { return state_ == HttpRequestParseState::kGotAll; }

    // 重置状态（处理 Keep-Alive 的下一个请求时使用）
    void reset() {
        state_ = HttpRequestParseState::kExpectRequestLine;
        HttpRequest dummy;
        request_.swap(dummy); // 清空 request_
    }

    /// 获取请求体
    const HttpRequest& request() const { return request_; }
    HttpRequest& request() { return request_; }

private:
    // 内部处理函数
    bool processRequestLine(const char* begin, const char* end);

    // 解析状态
    HttpRequestParseState state_;

    // 将来存储解析出来的请求体
    HttpRequest request_;
};



#endif //HTTPCONTEXT_H
