
#include <csignal>
#include <iostream>
#include <string>
#include <pthread.h>
#include "../MyWebServer/http/HttpRequest.h"
#include "../MyWebServer/http/HttpResponse.h"
#include "../MyWebServer/http/HttpServer.h"
#include "../MyWebServer/net/EventLoop.h"
#include "../MyWebServer/log/logger.h"
#include "../MyWebServer/base/utils.h"
#include "../MyWebServer/net/InetAddress.h"
#include <source_location>

namespace fs = std::filesystem;

// MIME Type 映射
std::string getMimeType(const std::string& filename) {
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos == std::string::npos) return "text/plain";

    std::string ext = filename.substr(dot_pos);
    if (ext == ".html") return "text/html";
    if (ext == ".css")  return "text/css";
    if (ext == ".js")   return "application/javascript";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg")  return "image/jpeg";
    if (ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".ico")  return "image/x-icon";
    if (ext == ".json") return "application/json";
    if (ext == ".txt")  return "text/plain";
    if (ext == ".mp4")  return "video/mp4";
    if (ext == ".pdf")  return "application/pdf";

    return "application/octet-stream"; // 默认二进制流
}


// 业务逻辑回调函数
void httpCallback(const HttpRequest& req, HttpResponse* resp) {
    // 1. 打印请求信息 (调试用，高并发压测时请注释掉日志)
    LOG_INFO("Request: {} {}", req.methodString(), req.url());
    LOG_INFO("User-Agent: {}", req.getHeader("User-Agent"));

    // 2. 简单的路由逻辑
    if (req.url() == "/") {
        resp->setStatusCode(HttpStatusCode::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("text/html");
        resp->setBody("<html>"
                      "<head><title>MyWebServer</title></head>"
                      "<body>"
                      "<h1>Welcome to MyWebServer!</h1>"
                      "<p>This is a high-performance C++ Web Server.</p>"
                      "<ul>"
                      "<li><a href='/hello'>/hello (Plain Text)</a></li>"
                      "<li><a href='/json'>/json (Mock JSON)</a></li>"
                      "<li><a href='/echo'>/echo (Echo Body)</a></li>"
                      "</ul>"
                      "</body>"
                      "</html>");
    }
    else if (req.url() == "/hello") {
        resp->setStatusCode(HttpStatusCode::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("text/plain");
        resp->setBody("Hello, World!");
    }
    else if (req.url() == "/json") {
        resp->setStatusCode(HttpStatusCode::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("application/json");
        resp->setBody(R"({"code": 0, "message": "success", "data": [1, 2, 3]})");
    }
    else if (req.url() == "/echo") {
        // 回显服务：将请求体原封不动返回

        LOG_INFO("客户端发送的请求体为：{}", req.getBody());

        resp->setStatusCode(HttpStatusCode::k200Ok);
        resp->setStatusMessage("OK");
        
        // 保持原有的 Content-Type，如果没有则默认为 text/plain
        std::string contentType = req.getHeader("Content-Type");
        if (contentType.empty()) {
            contentType = "text/plain";
        }
        resp->setContentType(contentType);
        
        // 设置响应体为请求体
        resp->setBody(req.getBody());
    }
    // --- 静态文件处理逻辑 ---
    // 约定：所有以 /static/ 开头的请求都映射到当前目录下的文件
    else if (req.url().find("/static/") == 0) {
        // 1. 获取文件路径 (去掉 /static/ 前缀，假设文件在当前运行目录)
        // 实际路径示例: ./test.txt
        std::string filename = req.url().substr(8);
        if (filename.empty()) filename = "index.html";

        // 2. 检查文件是否存在
        if (!fs::exists(filename) || fs::is_directory(filename)) {
            resp->setStatusCode(HttpStatusCode::k404NotFound);
            resp->setStatusMessage("Not Found");
            resp->setBody("File Not Found: " + filename);
            return;
        }

        // 3. 读取文件内容
        // 注意：这种方式会将整个文件读入内存。
        // 对于几百 MB 的文件，这会消耗大量内存。
        // 真正的高性能文件服务器通常使用 sendfile (零拷贝) 系统调用，
        // 但为了验证我们网络库的 Buffer 和 Write 逻辑，这里读入内存反而是个极好的测试case。
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            resp->setStatusCode(HttpStatusCode::k500InternalServerError);
            resp->setBody("Failed to open file");
            return;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::string body(size, '\0');
        if (file.read(&body[0], size)) {
            resp->setStatusCode(HttpStatusCode::k200Ok);
            resp->setStatusMessage("OK");
            resp->setContentType(getMimeType(filename));
            resp->setBody(body);
        } else {
            resp->setStatusCode(HttpStatusCode::k500InternalServerError);
            resp->setBody("Failed to read file");
        }
    }
    else {
        // 404 处理
        resp->setStatusCode(HttpStatusCode::k404NotFound);
        resp->setStatusMessage("Not Found");
        resp->setCloseConnection(true); // 404 时通常关闭连接
        resp->setBody("<html><body><h1>404 Not Found</h1></body></html>");
    }
}

int main(int argc, char* argv[]) {
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

    // 端口号，默认 9006
    int port = 9006;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    // 1. 创建主循环 (Main Reactor)
    EventLoop loop;
    setCurrentThreadName("HS-main"); // 设置当前线程的名称

    signal(SIGPIPE, SIG_IGN);

    // 2. 设置监听地址
    const InetAddress addr(port);

    // 3. 创建 HttpServer
    HttpServer server(&loop, addr, "HS", TcpServer::kReusePort, 8, 10.0);

    // 4. 设置业务回调
    server.setHttpCallback(httpCallback);

    // 5. 设置线程数量 (Sub Reactors)
    // 设为 0 表示单线程模式
    // 设为 4 表示 1个 Accept 线程 + 4个 IO 线程
    server.setThreadNum(4);

    // 6. 启动服务器
    server.start();

    LOG_INFO("HttpServer is running on port {}. Press Ctrl+C to stop.", port);

    // 7. 进入事件循环
    loop.loop();

    return 0;
}