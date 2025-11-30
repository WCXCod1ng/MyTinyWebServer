//
// Created by user on 2025/11/29.
//

#include "HttpServer.h"

#include "HttpContext.h"
#include "HttpResponse.h"
#include "log/logger.h"
#include "net/TcpConnection.h"
// 默认的 HTTP 回调（如果用户没设置）
void defaultHttpCallback(const HttpRequest&, HttpResponse* resp) {
    resp->setStatusCode(HttpStatusCode::k404NotFound);
    resp->setStatusMessage("Not Found");
    resp->setCloseConnection(true);
}

HttpServer::HttpServer(EventLoop *loop,
               const InetAddress &listenAddr,
               const std::string &name,
               const TcpServer::Option option,
               const size_t numThreads,
               const double idleTimeoutSeconds)
                   :server_(loop, listenAddr, name, option, numThreads, idleTimeoutSeconds),
                    httpCallback_(defaultHttpCallback)
{
    // 注册 TcpServer 的回调
    // 1. 连接建立/断开时 -> onConnection
    server_.setConnectionCallback(
        std::bind(&HttpServer::onConnection, this, std::placeholders::_1));

    // 2. 收到数据时 -> onMessage
    server_.setMessageCallback(
        std::bind(&HttpServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

HttpServer::~HttpServer() {
}

void HttpServer::start() {
    LOG_INFO("HttpServer[{}] starts listening on {}", server_.name(), server_.ipPort());
    server_.start();
}

void HttpServer::onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        LOG_INFO("Connection UP : {}", conn->peerAddress().toIpPort());

        // 【关键】为每个新连接创建一个 HttpContext
        // HttpContext 内部包含状态机和 HttpRequest 对象
        // 使用 std::any (setContext) 绑定到 TcpConnection 上
        conn->setContext(std::make_shared<HttpContext>());
    } else {
        LOG_INFO("Connection DOWN : {}", conn->peerAddress().toIpPort());
    }
}

void HttpServer::onMessage(const TcpConnectionPtr& conn, Buffer* buf, TimeStamp receiveTime) {
    // 1. 取出该连接对应的解析器 (Context)
    // 此时 context 可能是“全新的”，也可能是“解析了一半的”（针对分包情况）
    const auto context = conn->getContextValue<std::shared_ptr<HttpContext>>();

    // 2. 尝试解析 Buffer 中的数据
    if (!context->parseRequest(buf, receiveTime)) {
        // 解析失败（数据不够，或者格式错误）
        // 这里的 parseRequest 返回 false 只有一种可能：
        // 格式错误 -> 发送 400
        // 因为parseRequest内部会将数据不全认为正常（它会记录上一次请求的状态）

        // 为了简化，我们假设 parseRequest 内部处理了断点续传。
        // 如果这里直接返回 false，通常意味着请求格式是错误的（Bad Request）。
        conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");
        conn->shutdown();
    }

    // 3. 检查是否解析完了整个请求 (GotAll)
    if (context->gotAll()) {
        // 解析完成，调用业务逻辑
        onRequest(conn, context->request());

        // 【关键】重置 Context 状态机
        // 因为是 Keep-Alive，连接不会断，后续可能还有新的请求发过来
        context->reset();
    }
}

void HttpServer::onRequest(const TcpConnectionPtr& conn, const HttpRequest& req) {
    const std::string& connection = req.getHeader("Connection");
    // 判断是否长连接
    // HTTP/1.1 默认长连接，除非 Connection: close
    // HTTP/1.0 默认短连接，除非 Connection: Keep-Alive
    bool close = (connection == "close") ||
                 (req.getVersion() == Version::kHttp10 && connection != "Keep-Alive");

    // 构造响应对象
    HttpResponse response(close);

    // 调用用户回调 (业务逻辑)
    // 用户填充 response 的状态码、Header、Body
    httpCallback_(req, &response);

    // 序列化响应并通过网络发送
    Buffer buf;
    response.appendToBuffer(&buf);
    conn->send(&buf); // 此时数据进入 TcpConnection 的 OutputBuffer

    // 如果需要关闭连接，调用 shutdown
    // 注意：TcpConnection::shutdown 会等待数据发完再关闭
    if (response.closeConnection()) {
        conn->shutdown();
    }
}