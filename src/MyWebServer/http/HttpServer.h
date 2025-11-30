//
// Created by user on 2025/11/29.
//

#ifndef HTTPSERVER_H
#define HTTPSERVER_H
#include <functional>
#include <string>

#include "base/NonCopyable.h"
#include "net/TcpServer.h"

class InetAddress;
class EventLoop;
// 前向声明
class HttpRequest;
class HttpResponse;
class HttpContext;

class HttpServer : NonCopyable {
public:
    // 用户回调函数类型：当收到一个完整的 HTTP 请求时调用
    // 参数：(HttpRequest, HttpResponse*)
    // 用户需要从 Request 读取数据，并填充 Response
    using HttpCallback = std::function<void(const HttpRequest&, HttpResponse*)>;

    HttpServer(EventLoop *loop,
               const InetAddress &listenAddr,
               const std::string &name,
               const TcpServer::Option option = TcpServer::kReusePort,
               const size_t numThreads = 8,
               const double idleTimeoutSeconds = 60.0);

    ~HttpServer();

    EventLoop* getLoop() const { return server_.getLoop(); }

    // 设置处理 HTTP 请求的业务回调 (Controller)
    void setHttpCallback(const HttpCallback& cb) {
        httpCallback_ = cb;
    }

    // 设置线程池数量
    void setThreadNum(int numThreads) {
        server_.setThreadNum(numThreads);
    }

    // 启动服务器
    void start();

private:
    /// [内部回调] 当 TcpServer 有新连接时
    /// 当 TCP 三次握手完成后触发，会初始化HttpContext，使用上下文的原因是因为TCP协议是字节流，一个TCP连接上可能会跑多个HTTP请求
    void onConnection(const TcpConnectionPtr& conn);

    /// [内部回调] 当 TcpServer 收到数据时
    /// 核心逻辑：驱动 HttpContext 状态机解析数据
    /// 1. 从 context 里恢复上次的解析状态。
    /// 2. 喂数据给 parseRequest。
    /// 3. 如果解析出完整的请求（GotAll），就处理它。
    /// 4. 重要细节：如果 parseRequest 没解析完（比如 body 还没收全），函数会直接结束。Buffer 会保留现有的数据。等下次 onMessage 触发时，新的数据追加到 Buffer 后面，再次尝试解析。这就是非阻塞解析的精髓。
    /// 5. context->reset()：这一点非常关键。处理完一个请求后，必须把状态机重置为 ExpectRequestLine，以便处理该连接上的下一个 HTTP 请求
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, TimeStamp receiveTime);

    /// [内部辅助] 当解析完一个完整的 Request 后，处理并发送 Response
    /// 它会检查 Request Header 里的 Connection 字段，决定 Response 发完后要不要关连接。这是符合 HTTP/1.1 RFC 标准的行为
    /// 注意，它只会被onMessage调用（而且只有当请求解释完毕后才会调用）
    void onRequest(const TcpConnectionPtr& conn, const HttpRequest& req);

    /// 持有底层的 TcpServer
    TcpServer server_;
    /// 用户的业务逻辑回调
    HttpCallback httpCallback_;
};



#endif //HTTPSERVER_H
