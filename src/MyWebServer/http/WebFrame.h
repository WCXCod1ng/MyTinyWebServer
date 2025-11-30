//
// Created by user on 2025/11/30.
//

#ifndef WEBFRAME_H
#define WEBFRAME_H
#include <functional>

#include "Context.h"
#include "HttpServer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"

#include "WebRouter.h"

class WebFrame {
public:
    // WebFrame(EventLoop* loop, const InetAddress& addr, const std::string& name)
    //     : server_(loop, addr, name)
    // {
    //     // 将框架的 dispatch 方法注册给底层 HttpServer
    //     server_.setHttpCallback(
    //         std::bind(&WebFrame::dispatch, this, std::placeholders::_1, std::placeholders::_2));
    // }

    // 我们这里选择方式二，不使用外部的EventLoop，而是自行创建
    WebFrame(const InetAddress& addr, const std::string& name)
      : baseLoop_(),
        server_(&baseLoop_, addr, name)
    {
        server_.setHttpCallback(
            std::bind(&WebFrame::dispatch, this, std::placeholders::_1, std::placeholders::_2));

        // 初始化默认处理函数
        notFoundHandler_ = defaultNotFoundHandler;
        methodNotAllowedHandler_ = defaultMethodNotAllowedHandler;
        exceptionHandler_ = defaultExceptionHandler;
    }

    /// 启动服务
    void start() {
        // 启动HetpServer
        server_.start();
        // 启动主事件循环
        baseLoop_.loop();
    }

    /// 设置线程数
    void setThreadNum(const int num) { server_.setThreadNum(num); }

    // --- 路由注册接口 ---

    void GET(const std::string& path, HandlerFunc handler) {
        router_.add_route(path, Method::kGet, std::move(handler));
    }

    void POST(const std::string& path, HandlerFunc handler) {
        router_.add_route(path, Method::kPost, std::move(handler));
    }

    void PUT(const std::string& path, HandlerFunc handler) {
        router_.add_route(path, Method::kPut, std::move(handler));
    }

    void DELETE(const std::string& path, HandlerFunc handler) {
        router_.add_route(path, Method::kDelete, std::move(handler));
    }

    void HEAD(const std::string& path, HandlerFunc handler) {
        router_.add_route(path, Method::kHead, std::move(handler));
    }


    /// 自定义 404 处理
    void setNotFoundHandler(HandlerFunc handler) {
        notFoundHandler_ = std::move(handler);
    }

    /// 自定义 405 处理
    void setMethodNotAllowedHandler(HandlerFunc handler) {
        methodNotAllowedHandler_ = std::move(handler);
    }

    /// 自定义全局异常处理
    /// 注意：异常处理器的签名多了一个 exception 参数
    using ExceptionHandler = std::function<void(Context&, const std::exception&)>;
    void setExceptionHandler(ExceptionHandler handler) {
        exceptionHandler_ = std::move(handler);
    }

private:

    /// 核心分发逻辑 (Dispatcher)
    /// 这是所有业务的实际入口，会根据路径选择对应的handler并进行处理
    void dispatch(const HttpRequest& req, HttpResponse* resp) const {
        const auto &path = req.url();
        const auto method = req.method();
        // 调用底层的路由组件
        auto [status, handler, params] = router_.find_route(path, method);

        // 构建上下文
        Context ctx {req, resp, std::move(params)};

        if(status == RouteStatus::FOUND) {
            try {
                // 执行业务逻辑，传入 Context
                handler(ctx);
            } catch (const std::exception& e) {
                // 5. 捕获异常，调用全局异常处理器 (解决问题 1)
                if (exceptionHandler_) {
                    exceptionHandler_(ctx, e);
                }
            } catch (...) {
                // 捕获非 std::exception 异常
                // 这里可以做一个兜底
                resp->setStatusCode(HttpStatusCode::k500InternalServerError);
                resp->setBody("Unknown Internal Error");
            }

        } else if(status == RouteStatus::NOT_FOUND_METHOD) {
            // 路径匹配，但是没有对应的方法
            methodNotAllowedHandler_(ctx);
        } else {
            // 路径也不匹配
            notFoundHandler_(ctx);
        }
    }

    static void defaultNotFoundHandler(const Context& ctx) {
        ctx.resp()->setStatusCode(HttpStatusCode::k404NotFound);
        ctx.resp()->setStatusMessage("Not Found");
        ctx.resp()->setBody("404 Not Found");
        ctx.resp()->setCloseConnection(true);
    }

    static void defaultMethodNotAllowedHandler(const Context& ctx) {
        ctx.resp()->setStatusCode(HttpStatusCode::k405MethodNotAllowed);
        ctx.resp()->setStatusMessage("Method Not Allowed");
        ctx.resp()->setBody("405 Method Not Allowed");
        ctx.resp()->setCloseConnection(true); // 发生异常通常建议关闭连接
    }

    static void defaultExceptionHandler(const Context& ctx, const std::exception& e) {
        ctx.resp()->setStatusCode(HttpStatusCode::k500InternalServerError);
        ctx.resp()->setBody(std::string("Internal Server Error: ") + e.what());
        ctx.resp()->setCloseConnection(true); // 发生异常通常建议关闭连接
    }

    /// 主EventLoop，保证它的生命周期必须最长
    EventLoop baseLoop_;

    HttpServer server_; // 持有底层的 HttpServer
    // 路由组件
    WebRouter router_;


    // 保存用户的自定义处理器
    HandlerFunc notFoundHandler_;
    HandlerFunc methodNotAllowedHandler_;
    ExceptionHandler exceptionHandler_;
};



#endif //WEBFRAME_H
