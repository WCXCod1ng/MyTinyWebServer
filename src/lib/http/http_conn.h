//
// Created by user on 2025/11/11.
//

#ifndef HTTP_CONN_H
#define HTTP_CONN_H


#include <format>
#include <functional>
#include <iostream>
#include <memory>

// 一个模拟的HTTP连接类
class HttpConnection : public std::enable_shared_from_this<HttpConnection> {
public:
    // 为了简化，构造函数直接接收必要的外部依赖
    HttpConnection(int sockfd, class ThreadPool* pool)
        : m_sockfd(sockfd), m_pool(pool) {}

    // void process() {
    //     // 模拟处理读写和业务逻辑
    //     // 在真实场景中，这里会解析HTTP请求，查询数据库，然后准备响应
    // }
    static int handle_read(std::shared_ptr<HttpConnection> conn) {
        std::cout << std::format("处理read操作 已连接socket {}",(*conn).m_sockfd) << std::endl;

        return 0;
    }
    static int handle_write(std::shared_ptr<HttpConnection> conn) {
        std::cout << std::format("处理write操作 已连接socket {}",(*conn).m_sockfd) << std::endl;

        return 0;
    }

    int get_sockfd() const { return m_sockfd; }

private:
    int m_sockfd;
    class ThreadPool* m_pool; // 假设需要线程池来执行异步任务
    // ... 其他成员，如读写缓冲区等
};


#endif //HTTP_CONN_H
