//
// Created by user on 2025/11/30.
//

#ifndef CONTEXT_H
#define CONTEXT_H
#include "HttpRequest.h"
#include "HttpResponse.h"


/// 暴露给业务端用户使用的Context
class Context {
public:
    Context(const HttpRequest& req, HttpResponse* resp,
            std::unordered_map<std::string, std::string> params)
        : req_(req), resp_(resp), params_(std::move(params)) {}

    /// 获取路径参数 /user/:id -> getParam("id")
    /// 不存在则返回空
    std::optional<std::string> pathVariable(const std::string& key) const {
        if(const auto it = params_.find(key); it != params_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /// 获取查询参数 ?name=abc -> query("name") == abc
    std::optional<std::string> query(const std::string& key) const {
        const auto &queries = req_.getQueries();
        if(const auto it = queries.find(key); it != queries.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /// 获取请求头
    std::optional<std::string> header(const std::string& key) const {
        const auto &headers = req_.headers();
        if(const auto it = headers.find(key); it != headers.end()) {
            return it->second;
        }
        return std::nullopt;
    }


    // 获取原始请求体
    const HttpRequest& req() const { return req_; }
    // 获取原始响应体
    HttpResponse* resp() const { return resp_; }


    // --- 响应辅助方法 (类似于) ---

    /// 设置响应头
    void setHeader(const std::string& key, const std::string& value) {
        resp_->addHeader(key, value);
    }

    /// 以字符串方式响应
    void STR(const HttpStatusCode code, const std::string& str) const {
        resp_->setStatusCode(code);
        resp_->setContentType("text/plain");
        resp_->setBody(str);
    }

    /// 以json方式响应
    void JSON(const HttpStatusCode code, const std::string& jsonStr) const {
        resp_->setStatusCode(code);
        resp_->setContentType("application/json");
        resp_->setBody(jsonStr);
    }

    /// 以对象方式响应
    /// 内部会自动转化为字符串
    template<typename T>
    void OBJ(const HttpStatusCode code, const T& obj) const {
        resp_->setStatusCode(code);
        resp_->setContentType("application/json");
        resp_->setBody(obj);
    }

    // HTML, File 等辅助方法...

private:
    const HttpRequest& req_;
    HttpResponse* resp_;
    std::unordered_map<std::string, std::string> params_; // 存储WebRouter解析出来的请求参数，包括路径参数、查询字符串、通配符字符串等
};


#endif //CONTEXT_H
