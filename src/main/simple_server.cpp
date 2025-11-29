#include <string>
#include <functional>

#include "base/ThreadPool.h"
#include "base/Buffer.h"
#include "base/TimeStamp.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/Callbacks.h"
#include "net/TcpConnection.h"
#include "net/TcpServer.h"
#include "log/logger.h"
// #include <source_location>



class EchoServer {
public:
    EchoServer(EventLoop* loop, const InetAddress& addr, const std::string& name)
        : server_(loop, addr, name),
          loop_(loop)
    {
        // 注册回调函数
        server_.setConnectionCallback(
            std::bind(&EchoServer::onConnection, this, std::placeholders::_1));

        server_.setMessageCallback(
            std::bind(&EchoServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        // 设置线程数量 (3个 SubReactor + 1个 MainReactor)
        server_.setThreadNum(3);

        // 设置连接的存储时间为10秒
        server_.setIdleTimeoutSeconds(10.0);
    }

    void start() {
        server_.start();
    }

private:
    // 连接建立或断开的回调
    void onConnection(const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            LOG_INFO("Connection UP : {} from {}", conn->name(), conn->peerAddress().toIpPort());
        } else {
            LOG_INFO("Connection DOWN : {}", conn->name());
        }
    }

    // 收到消息的回调
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, TimeStamp time) {
        // 从 Buffer 中取出所有数据转为 string
        std::string msg = buf->retrieveAllAsString();

        // 打印收到数据（测试用，生产环境请勿打印大量 IO 日志）
        LOG_INFO("Proxy: {} bytes received at {}", msg.size(), time.toString());

        // 回显：把收到的数据原封不动发回去
        conn->send(msg);

        // 如果想测试关闭连接，可以判断 msg == "quit\n"
        // if (msg == "quit\n") {
        //     conn->shutdown();
        // }
    }

    TcpServer server_;
    EventLoop* loop_;
};

int main() {
    // 开启日志，级别为 INFO
    Logger::Config log_config;
    log_config.log_folder = "/root/code/cpp/MyTinyWebServer/out/log"; // 日志文件存储的路径
    log_config.max_queue_size = 1024;     // 开启异步日志
    LogLevel default_level = LogLevel::INFO; // 设置默认日志等级
    // note 在debug的时候默认设置为true，方便debuug
    log_config.is_override = true;
    log_config.enable_console_sink = true;
    log_config.flush_interval_seconds = 0; // 同步刷新
    Logger::get_instance().init(log_config);

    // 创建主事件循环 (Main Reactor)
    EventLoop loop;
    setCurrentThreadName("ES-main"); // 设置当前线程的名称

    // 监听地址
    InetAddress addr(8080);

    // 创建服务器
    EchoServer server(&loop, addr, "ES");

    // 启动服务器
    server.start();

    // // 执行一个定时任务
    // loop.runEvery(5, []() {
    //    LOG_INFO("Server is alive");
    // });

    // 开启循环
    loop.loop();

    return 0;
}
