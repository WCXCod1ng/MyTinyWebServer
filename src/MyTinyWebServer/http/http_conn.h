//
// Created by user on 2025/11/11.
//

#ifndef HTTP_CONN_H
#define HTTP_CONN_H


#include <atomic>
#include <format>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <filesystem>
#include <sys/mman.h>

#include "http_define.h"
#include "router.h"
#include "mysql_connection_pool/mysql_connection_pool.h"

// HTTP连接类，负责处理HTTP相关的连接、协议等
class HttpConnection : public std::enable_shared_from_this<HttpConnection> {
public:
    enum class State {
        READING,
        WRITING
    };

    // 定义一个回调函数类型
    using EpollModifier = std::function<void(int sockfd, uint32_t events)>;


private:
    /// 定义写状态
    enum class WriteStatus {
        SUCCESS, // 成功
        AGAIN, // 需要继续写
        FATAL_ERROR // 致命错误，需要关闭
    };


    /// 定义内部的状态机的状态
    enum class ParseState {
        REQUEST_LINE, // 当前正在处理请求行
        HEADERS, // 当前正在处理请求头
        CONTENT, // 当前正在处理请求体
        COMPLETE // 请求处理完毕
    };

public:
    /// 通过构造函数接受所有必要的外部资源：依赖注入
    // HttpConnection(int sockfd, const sockaddr_in& addr, const std::string& doc_root, mysql_conn_pool& sql_pool, const Router& router, EpollModifier epoll_modifier);
    HttpConnection(int sockfd, const sockaddr_in& addr, const std::string& doc_root, const Router& router, EpollModifier epoll_modifier);

    /// 使用析构函数处理RAII风格的资源清理
    ~HttpConnection();

    /// 主处理函数，由WebServer管理的线程池调用
    /// WebServer不需要知道HttpConnection的内部工作流程（是读？是解析？还是写？）。它只下达一个命令：“处理这个连接”。process内部会根据当前状态决定是调用handle_read还是build_response等
    void process(bool isET);

    /// 获取该HTTP连接所对应的socket文件描述符，也由WebServer调用
    int get_sockfd() const;

    /// 关闭连接，由WebServer调用
    void close_connection();

private:
    // 处理socket的IO
    // 职责分离。将网络I/O的细节与HTTP解析/构建的逻辑分开。
    /// 处理IO读操作
    /// 如果返回true， 表示数据成功读完（可能读到了数据，也可能本次没数据可读），且连接仍然有效；
    /// 如果返回false，则表示对方关闭或者发生错误，需要关闭连接。
    bool handle_read(bool isET);
    /// 处理IO写操作：将响应缓冲区中的数据写入socket
    WriteStatus handle_write(bool isET);

    // 解析HTTP请求
    /// 解析整个HTTP请求
    /// 返回true表示解析完成或者还有数据要取（没有出错）；
    /// 返回false表示不符合HTTP的协议，应当处理错误
    bool parse_request();
    /// 解析请求行："GET /index.html HTTP/1.1"
    bool parse_request_line(std::string_view line);
    /// 解析请求头
    bool parse_header(std::string_view line);

    /// 解析查询字符串：query params，并将其填入m_requests.query_params中
    /// 按照“&”分割成一个个的key=value对
    /// 返回值：是否解析成功
    bool parse_query_params(std::string_view query_string);

    // 处理实际的请求
    /// 从m_request中读取内容（需要保证前面的内容已经被处理，可以保证，一个HttpConnection同时只由一个线程处理）
    /// 将处理结果写入m_response中
    /// 处理过程中可能抛出异常（异常的原因可能是WebServer框架，也可能是业务代码，因此需要catch）
    void handle_request();

    static std::string_view trim(std::string_view content);


    // 构建HTTP响应
    // 进一步的职责细分。根据请求的类型（静态文件、动态CGI、错误），调用不同的函数来处理，使代码逻辑更加清晰
    /// 构建整个HTTP响应体
    // void build_response();
    /// 处理静态请求（例如访问静态资源），它不仅进行请求处理，还直接构造响应
    /// 返回值：true表示处理静态请求成功；false表示处理静态请求失败。无论成功或失败，它都已经将请求体构造好了（因此后续不再需要build_response）
    bool handle_static_request_and_response();
    // /// 构建api响应，它会直接根据m_response构造响应体
    // void build_dynamic_response();
    /// 构建静态错误响应
    void build_static_error_response(myhttp::HttpCode code, std::string_view message);
    /// 构建动态错误响应
    void build_json_error_response(myhttp::HttpCode code, std::string_view message);


    /// 重置连接（支持长连接功能）
    void reset();

    /// 返回是否是 keep-alive连接（长连接）
    bool is_keep_alive() const;

    // socket 连接信息
    // socket描述符
    int m_sockfd;
    // 客户端地址信息
    sockaddr_in m_address;
    std::atomic<bool> m_is_closed = false;

    // 静态资源根目录
    std::filesystem::path m_doc_root;

    // // 数据库连接池
    // mysql_conn_pool& m_sql_pool;

    // 读写缓冲区，替代了之前的char[]数组，可以不用担心缓冲区溢出或者忘记delete
    std::string m_read_buffer;
    std::string m_write_buffer;

    // HTTP请求解析状态，将被用于驱动内部的状态机
    ParseState m_parse_state;
    // 构建结构体表示HTTP请求，它代表一份请求头
    myhttp::HttpRequest m_request;

    /// 新增一个Router类用于处理路由
    const Router& m_router;

    // 构建结构体表示HTTP响应，它代表一份请求体
    myhttp::HttpResponse m_response;

    /// 当前应当处于读操作还是写操作，初始时刻为读操作
    State m_connection_state = State::READING;


    // 写操作状态
    size_t m_bytes_to_send = 0;
    size_t m_bytes_have_sent = 0;

    // 存储回到函数用以让通知WebServer修改epoll的状态
    EpollModifier m_epoll_modifier;
};

// 为 std::formatter<HttpConnection::Method> 进行模板特化，使HttpConnection::Method支持format方法
template <>
struct std::formatter<myhttp::Method> {
    // 2. 实现 parse 函数
    //    对于这个简单例子，我们不支持任何特殊的格式说明符，
    //    所以 parse 函数非常简单，只需找到格式字符串的结尾 '}' 即可。
    constexpr auto parse(std::format_parse_context& ctx) {
        // ctx.begin() 指向格式说明符的开始（或 '}'）
        // ctx.end() 指向格式字符串的结尾 '}'
        auto it = ctx.begin();
        // 确保在 '}' 之前没有我们不认识的格式
        if (it != ctx.end() && *it != '}') {
            throw std::format_error("invalid format specifier for Point");
        }
        return it; // 返回解析结束的位置
    }

    // 3. 实现 format 函数
    //    这个函数是实际进行格式化的地方
    auto format(const myhttp::Method& m, std::format_context& ctx) const {
        // ctx.out() 是一个输出迭代器，我们可以向它写入内容
        // 最简单的方式是使用 std::format_to 将结果写入这个迭代器
        return std::format_to(ctx.out(), "{}", serialize_method_kind(m));
    }
};


#endif //HTTP_CONN_H
