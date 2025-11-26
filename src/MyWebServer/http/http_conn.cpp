//
// Created by user on 2025/11/11.
//

#include "http_conn.h"

#include <fcntl.h>
#include <map>
// #include <bits/fs_path.h>
#include <sstream>
#include <filesystem>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "log/logger.h"
#include "utils/error_handler.h"
#include "utils/url.h"

// HttpConnection::HttpConnection(int sockfd, const sockaddr_in &addr, const std::string& doc_root, mysql_conn_pool &sql_pool, const Router& router, EpollModifier epoll_modifier):
HttpConnection::HttpConnection(int sockfd, const sockaddr_in &addr, const std::string& doc_root, const Router& router, EpollModifier epoll_modifier):
    m_sockfd(sockfd),
    m_address(addr),
    m_doc_root(std::filesystem::path(doc_root)),
    // m_sql_pool(sql_pool),
    m_router(router),
    m_parse_state(ParseState::REQUEST_LINE),
    m_epoll_modifier(std::move(epoll_modifier))
{

}

HttpConnection::~HttpConnection() {
    // 为什么不 close(m_sockfd)?
    // 非常重要的所有权 (Ownership) 问题。HttpConnection 使用 socket，但它不拥有 socket。
    // m_sockfd 是由WebServer通过accept()创建的，它的生命周期应该由WebServer来管理。当WebServer决定关闭一个连接时（因为超时、客户端关闭或服务器关闭），它会负责调用close(sockfd)，然后从它的连接管理容器（m_connections map）中移除对应的HttpConnection智能指针。智能指针的引用计数归零，才会触发HttpConnection的析构。
    // 如果析构函数也调用close()，会导致同一个文件描述符被关闭两次，这是一个严重的错误
    close_connection();
}

void HttpConnection::process(const bool isET) {
    LOG_INFO("线程池拿到这样一个线程");
    // 如果已经关闭，直接退出
    if(m_is_closed) {
        return;
    }

    // ================== 状态一：读取和处理请求 ==================
    if(m_connection_state == State::READING) {
        // 第一步：读取数据
        // handle_read会读取socket上的所有可用数据到m_read_buffer
        if (!handle_read(isET)) {
            // 读取失败或连接已关闭
            // WebServer在接收到这个信号后会关闭连接
            return;
        }

        // 第二步：解析请求
        // parse_request会尝试从m_read_buffer中解析出一个完整的HTTP请求
        if (!parse_request()) {
            // 解析失败，说明请求格式有误 (Bad Request)
            build_static_error_response(myhttp::HttpCode::BAD_REQUEST, "Your request has bad syntax.");
        } else {
            // 解析成功
            // 第三步：处理请求，并返回响应体。
            handle_request();
            // // build_response会根据解析出的m_request内容，填充m_response结构体
            // build_response();
        }

        // 第四步：准备写入的数据，也即IO写操作
        // 将构建好的响应头（和可能的短响应体）序列化到 m_write_buffer
        // 这是为了使用 writev 做准备
        std::ostringstream response_stream;
        response_stream << "HTTP/1.1 " << static_cast<int>(m_response.status_code) << " " << myhttp::HttpCodeExplanations.at(m_response.status_code) << "\r\n";
        for (const auto& [key, value] : m_response.headers) {
            response_stream << key << ": " << value << "\r\n";
        }
        response_stream << "\r\n";
        if (!m_response.body.empty()) {
            response_stream << m_response.body;
        }
        m_write_buffer = response_stream.str();

        // 计算总共需要发送的字节数
        m_bytes_to_send = m_write_buffer.size() + (m_response.mmapped_file.addr ? m_response.mmapped_file.size : 0);

        // 状态切换到写状态
        m_connection_state = State::WRITING;
    }

    // ================== 状态二：写入响应 ==================
    // 注意：这里没有 "else if"，是故意的。
    // 从READING状态处理完后，会立即尝试写入，这是一个优化。
    if(m_connection_state == State::WRITING) {
        // 第五步：尝试直接写入
        // 根据写返回的状态，确定如何处理
        switch (handle_write(isET)) {
            case WriteStatus::SUCCESS:
                // 数据全部发送成功
                    if (is_keep_alive()) {
                        // 如果是长连接，重置状态并告诉WebServer继续监听读事件
                        reset();
                        m_epoll_modifier(m_sockfd, EPOLLIN);
                    } else {
                        // 如果是短连接，标记此连接，等待WebServer清理
                        m_is_closed.store(true);
                    }
            break;

            case WriteStatus::AGAIN:
                // 内核缓冲区满，告诉WebServer重新监听写事件
                    m_epoll_modifier(m_sockfd, EPOLLOUT);
            break;

            case WriteStatus::FATAL_ERROR:
                // 发生致命写入错误，标记此连接，等待WebServer清理
                    m_is_closed.store(true);
            break;
        }
    }
}

int HttpConnection::get_sockfd() const {
    return m_sockfd;
}

void HttpConnection::close_connection() {
    if(!m_is_closed) {
        m_is_closed.store(true);
        // 值得注意的是，这里不能调用close，关闭socket的操作应该由WebServer负责，这里只更新对象内部的状态

        // *** 主动释放mmap资源 ***
        // 也可以交由MmappedFile的析构函数来实现，但是会被延迟释放，这里的目的是主动释放
        if (m_response.mmapped_file.addr != nullptr) {
            munmap(m_response.mmapped_file.addr, m_response.mmapped_file.size);
            m_response.mmapped_file.addr = nullptr;
            m_response.mmapped_file.size = 0;
            LOG_DEBUG("Mmapped file for fd %d unmapped in close_connection.", m_sockfd);
        }
    }
}


/// 如果返回true， 表示数据成功读完（可能读到了数据，也可能本次没数据可读），且连接仍然有效；
/// 如果返回false，则表示对方关闭或者发生错误，需要关闭连接。
bool HttpConnection::handle_read(const bool isET) {
    LOG_DEBUG("处理读IO操作");
    // 处理连接关闭的情况
    if(m_is_closed) {
        return false;
    }

    // 临时缓冲区来循环读取
    char buffer[2048];

    // note 这里使用ET模式，因此需要用一个while循环来重复读取直到所有内容都被读取完毕
    do {
        // 从socket中读取数据到临时buffer中
        // 在这里它实际上就等价于recv(m_sockfd, buffer, sizeof(buffer), 0)，最后一个参数是一些配置选项，如果只是简单地将其读出来那么就可以设置为0，此时功能与read等价
        ssize_t bytes_read = read(m_sockfd, buffer, sizeof(buffer));

        // 根据返回值解析读取的结果
        if(bytes_read > 0) {
            // 如果返回值 > 0，说明成功读取到结果，将其追加到read_buffer中，并且还没有读取完，应当继续读
            m_read_buffer.append(buffer, bytes_read);
        } else if(bytes_read == 0) {
            // 读取到了EOF，是因为客户端发送了FIN，说明read结束，将连接标记为关闭并返回false
            return false;
        } else {
            // 发生错误

            // 对于非阻塞IO，出现这个错误表示数据已经读取完毕，跳出循环，read结束
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // —— ET 模式：读到 EAGAIN 必须退出循环
                // —— LT 模式：读到 EAGAIN 表示当前无更多数据，但之后还会触发
                break;
            }

            // 否则是其他错误，此时要关闭当前连接
            m_is_closed.store(true);

            return false;
        }
    } while (isET);

    LOG_DEBUG("IO读取正常");
    // 到此说明读取正常，返回true
    return true;
}

HttpConnection::WriteStatus HttpConnection::handle_write(const bool isET) {
    LOG_DEBUG("处理写IO操作，总计需要写入 {}字节", m_bytes_to_send);
    if (m_is_closed) {
        return WriteStatus::FATAL_ERROR;
    }

    // 兼容ET模式和LT模式
    // 对于ET模式，循环写入，直到遇到EAGAIN或其他错误
    // 对于LT模式，只写入一次
    do {
        // *** 核心改进：在每次循环迭代时都重新构建 iovec ***
        // note 使用writev的意义在于减少IO的开销：
        // 由于相应头和响应体在不连续的两块内存（buffer和mmap产生的内存中），使用常规的write或send需要调用两次——产生两次系统调用开销。而将他们拷贝到一段连续的内存中的效率更低——mmap的文件可能非常大，占用大量CPU和IO时间
        // writev允许将两段不连续的内存发送出去，操作系统会将他们看成是连续的数据流
        struct iovec iv[2];
        int iv_count = 0;

        // 1. 计算响应头还剩下多少未发送
        size_t headers_total_size = m_write_buffer.size();
        // 计算已经发送的header字节数，最多是headers_total_size个
        size_t headers_sent = (m_bytes_have_sent < headers_total_size) ? m_bytes_have_sent : headers_total_size;
        size_t headers_left_to_send = headers_total_size - headers_sent;

        // 响应头还未发送完毕，则需要更新iovec数组的指针和长度
        if (headers_left_to_send > 0) {
            iv[iv_count].iov_base = &m_write_buffer[0] + headers_sent; // 移动到下一个未发送的位置
            iv[iv_count].iov_len = headers_left_to_send; // 更新需要发送的长度
            iv_count++; // 表示writev需要考虑响应头中的数据
        }

        // 2. 计算文件内容还剩下多少未发送
        if (m_response.mmapped_file.addr != nullptr && m_response.mmapped_file.size > 0) {
            size_t file_total_size = m_response.mmapped_file.size;
            // 只有在头部完全发送后，才开始计算文件已发送的字节数：因此需要减去header的大小
            size_t file_sent = (m_bytes_have_sent > headers_total_size) ? (m_bytes_have_sent - headers_total_size) : 0;
            size_t file_left_to_send = file_total_size - file_sent;

            if (file_left_to_send > 0) {
                iv[iv_count].iov_base = static_cast<char*>(m_response.mmapped_file.addr) + file_sent;
                iv[iv_count].iov_len = file_left_to_send;
                iv_count++;
            }
        }

        // 如果没有数据需要发送了（可能因为上一次循环刚好发完），退出
        if (iv_count == 0) {
            m_bytes_to_send = 0; // 确保状态同步
            break;
        }

        // 调用writev
        ssize_t bytes_written = writev(m_sockfd, iv, iv_count);

        if (bytes_written < 0) {
            // 对于非阻塞模式的ET方式，EAGAIN（或者EWOULDBLOCK）实际上是正常退出的唯一出口，表示可以退出（但是EPOLLOUT还需要重新监听以等待剩余未发送的数据）
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return WriteStatus::AGAIN; // 内核缓冲区满，需要等待
            }
            LOG_ERROR("Write error on fd: %d, errno: %d (%s)", m_sockfd, errno, strerror(errno));
            return WriteStatus::FATAL_ERROR; // 发生致命错误
        }

        // 3. 只更新权威的状态变量
        m_bytes_have_sent += bytes_written;
        m_bytes_to_send -= bytes_written;

    } while (isET && m_bytes_to_send > 0); // ET模式下且还有数据时继续循环

    // 循环结束后 (只可能在LT模式下发生)
    // 检查是否还有数据待发送
    if (m_bytes_to_send > 0) {
        // 在LT模式下，如果没写完但没遇到EAGAIN，
        // 我们假设下次epoll还会通知我们，所以返回AGAIN是合理的，
        // process会去注册EPOLLOUT，确保我们不会错过写入机会。
        return WriteStatus::AGAIN;
    } else {
        // 如果数据已经写完，就可以直接返回了（不需要再次AGAIN了）
        // 如果再次AGAIN，process就会重新注册EPOLLOUT，虽然没有影响，但是是冗余操作（本来系统就会重新出发EPOLLOUT事件）
        return WriteStatus::SUCCESS;
    }
}

bool HttpConnection::parse_request() {
    LOG_DEBUG("解析请求");
    size_t line_end_pos;
    std::string_view line;
    while(m_parse_state != ParseState::COMPLETE) {
        // 枚举当前的状态，选择处理对应的处理模式
        switch (m_parse_state) {
            case ParseState::REQUEST_LINE:
                // 提取一行，标志是：“\r\n”
                line_end_pos = m_read_buffer.find("\r\n");
                if(line_end_pos == std::string_view::npos) {
                    // 没有完整的行，需要更多数据
                    return true;
                }
                // 注意，line没有将“\r\n”包括在内
                line = {m_read_buffer.data(), line_end_pos};
                LOG_DEBUG("解析请求行，缓冲区中内容为：{}", m_read_buffer.substr(0, line_end_pos));
                // 调用实际的处理函数
                if(!parse_request_line(line)) {
                    return false;
                }
                // 到此说明处理成功，切换到下一个状态
                m_parse_state = ParseState::HEADERS;
                // 从缓冲区中移除这一行（还有换行符）
                m_read_buffer.erase(0, line_end_pos + 2);
                break;
            case ParseState::HEADERS:
                // 同样也读取一行
                line_end_pos = m_read_buffer.find("\r\n");
                if(line_end_pos == std::string_view::npos) {
                    // 没有完整的行，需要更多数据
                    return true;
                }
                line = {m_read_buffer.data(), line_end_pos};

                if(line.empty()) {
                    // 如果是空行，表示头部结束，应该转到CONTENT状态
                    m_parse_state = ParseState::CONTENT;
                    LOG_DEBUG("请求头为空");
                } else {
                    // 否则调用parse_header依次解析每一个header
                    if(!parse_header(line)) {
                        return false;
                    }
                }
                LOG_DEBUG("解析请求头，请求头结果为：{}", m_read_buffer.substr(0, line_end_pos));
                // 从缓冲区中移除这一行（还有换行符）
                m_read_buffer.erase(0, line_end_pos + 2);
                break;
            case ParseState::CONTENT:
                if(const auto it = m_request.headers.find("content-length"); it != m_request.headers.end()) {
                    LOG_DEBUG("解析请求体");
                    // 读取请求体
                    try {
                        // 数据长度
                        size_t content_length = std::stoll(it->second);
                        if(m_read_buffer.size() < content_length) {
                            // 请求体不完整，需要等待下一次提取
                            return true;
                        } else {
                            // 一次将结果都读取出来，并清空buffer
                            m_request.body = m_read_buffer.substr(0, content_length);
                            m_read_buffer.erase(0, content_length);
                            LOG_DEBUG("解析成功");
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("读取请求体时出错，请求体长度为{}，错误原因是：{}", it->second, e.what());
                        return false;
                    }
                } else {
                    // 没有content-length就默认没有请求体？
                    LOG_DEBUG("没有请求体");
                }
                // 解析成功
                // 到此执行结束，设置为COMPLETE，后续会通过外层的while退出循环
                m_parse_state = ParseState::COMPLETE;
                break;
            default:
                // continue 让外层的while判断结束，这里的break支持跳出switch的循环
                break;
        }
    }

    // 到此退出说明解析完成
    return true;
}

/// 解析请求行: "GET /index.html HTTP/1.1"
bool HttpConnection::parse_request_line(std::string_view line) {
    // 1. 查找第一个空格，分离出Method
    const size_t method_end = line.find(' ');
    if(method_end == std::string_view::npos) {
        // 如果找不到method则解析失败
        return false;
    }
    std::string_view method_sv = line.substr(0, method_end);
    if(method_sv == "GET") {
        m_request.method = myhttp::Method::GET;
    } else if(method_sv == "POST") {
        m_request.method = myhttp::Method::POST;
    } else {
        m_request.method = myhttp::Method::UNKNOWN;
    }

    // 2. 查找第二个空格，理处URI+Query：从method的结尾到下一个空格的位置是URI+Query
    const size_t uri_end = line.find(' ', method_end+1);
    if(uri_end == std::string_view::npos) {
        // 找不到uri则解析失败
        return false;
    }
    // 有没有Query需要继续判断，这里提取到的是所有的结果
    std::string_view full_uri = line.substr(method_end+1, uri_end - (method_end+1));
    // 寻找"?"来分离path和query string
    size_t query_start = full_uri.find('?');
    if(query_start != std::string_view::npos) {
        // 找到了？，那么？之前的是uri
        m_request.uri = full_uri.substr(0, query_start);
        // ？之后的是查询参数（query params），将它提取出来
        const std::string_view query_string = full_uri.substr(query_start + 1);
        // 交给专门处理查询query params的函数处理
        parse_query_params(query_string);
    } else {
        // 没有找到，则整个都是URI
        m_request.uri = full_uri;
    }

    // 3. 剩余认为是Version：从URI的结尾到最后
    m_request.version = line.substr(uri_end+1);

    // fixme 没有考虑原始代码中的http://和https://
    // 验证URI和Version
    if(m_request.uri.empty() || m_request.uri[0] != '/') {
        return false;
    }
    if(m_request.version != "HTTP/1.1" && m_request.version != "HTTP/1.0") {
        LOG_ERROR("Unsupported HTTP version: %s", m_request.version.c_str());
        return false;
    }

    LOG_DEBUG("解析成功：请求方法={}，URI={}，协议版本号={}", m_request.method, m_request.uri, m_request.version);
    // 解析成功
    return true;
}

// 解析单个头部字段: "Host: localhost:8080"
bool HttpConnection::parse_header(std::string_view line) {
    // 查找第一个“:”
    const size_t colon_pos = line.find(':');
    if(colon_pos == std::string_view::npos) {
        return false;
    }

    // 提取key和value
    std::string_view key_sv = line.substr(0, colon_pos);
    std::string_view value_sv = line.substr(colon_pos+1);

    // Key周围不能有空格，协议要求“:”前的内容不能包含任何空格
    if(key_sv.find_first_of(" \t\n\r\f\v") != std::string_view::npos) {
        return false;
    }
    // 清理Value周围的空格
    value_sv = trim(value_sv);
    if(value_sv.empty()) {
        return false;
    }

    // HTTP的头部不区分大小写，统一转化为小写
    std::string key;
    key.resize(key_sv.size());
    std::transform(key_sv.begin(), key_sv.end(), key.begin(), tolower);

    // 添加到m_request中，作为一个key-value entry
    m_request.headers[key] = std::string(value_sv);

    return true;
}

bool HttpConnection::parse_query_params(std::string_view query_string) {
    std::size_t start = 0;
    while(start < query_string.size()) {
        std::size_t ampersand_pos = query_string.find_first_of('&', start);
        if(ampersand_pos == std::string_view::npos) {
            ampersand_pos = query_string.size(); // 默认放到结尾，方便start统一处理
        }
        // 这样[start, ampersand_pos]之间就是一个完整的key=value对
        std::string_view entry = query_string.substr(start, ampersand_pos - start);

        // 更新start的位置为“&”的下一个位置，ampersand_pos已经特判最后一个字符了
        start = ampersand_pos + 1;

        // 如果entry为空，跳过
        if(entry.empty()) {
            continue;
        }

        // 解析key和value
        const std::size_t equal_pos = entry.find_first_of('=');
        std::string_view key_sv;
        std::string_view value_sv;
        if(equal_pos == std::string_view::npos) {
            // note 找不到“=”，则直接认为是只有key，默认value为空字符串
            key_sv = entry.substr(0);
            value_sv = "";
        } else {
            // 第一个“=”之前的被认为是key，第一个“=”之后的被认为是value。这是行业的一般遵循标准
            key_sv = entry.substr(0, equal_pos);
            value_sv = entry.substr(equal_pos + 1);
        }

        // note 生产环境需要对key和value进行解码（因为可能会出现非ASCII字符或“/”、“&”、“.”等特殊字符）

        auto key = url_decode(key_sv);
        auto value = url_decode(value_sv);

        // 加入到查询参数集合中
        m_request.query_params[std::move(key)] = std::move(value);
    }

    return true; // 除非未来有严格的格式要求，否则总是返回 true
}

void HttpConnection::handle_request() {
    // 到此一定满足：1. 解析完成， 2. m_request中存储了我们想要的请求体
    LOG_DEBUG("uri = {}, method = {}, version = {}", m_request.uri, m_request.method, m_request.version);

    // 1. 从请求体中获取URL与方法，并进行路由
    auto [route_status, handler, params] = m_router.find_route(m_request.uri, m_request.method);

    switch (route_status) {
        case RouteStatus::NOT_FOUND_URL:
            // 作为api请求时，发现路径不匹配，现在尝试作为静态资源
            if(!handle_static_request_and_response()) {
                // 如果作为静态资源请求也失败，则返回真正的失败
                // 事实上什么也不需要做，因为handle_static_request_and_response已经构建响应结果了（无论成功或者失败）
            }
            // // std::string_view a = myhttp::HttpCodeExplanations[myhttp::HttpCode::NOT_FOUND]; // 报错，因为[]不是一个const成员函数，而at有一个const成员版本（不存在则抛异常）
            // build_error_response(myhttp::HttpCode::NOT_FOUND, myhttp::HttpCodeExplanations.at(myhttp::HttpCode::NOT_FOUND));
            break;
        case RouteStatus::NOT_FOUND_METHOD:
            // 作为api请求时路径匹配，但是方法不匹配
            build_static_error_response(myhttp::HttpCode::METHOD_NOT_ALLOWED, myhttp::HttpCodeExplanations.at(myhttp::HttpCode::METHOD_NOT_ALLOWED));
            break;
        case RouteStatus::FOUND:
            // 路径匹配，进行实际的业务处理
            try {
                handler(m_request, m_response);
                // 执行到此说明没有发生异常，那么就需要使用构建正常的响应对象，已经构建好了
            } catch (std::exception& e) {
                // 处理业务时可能出现异常，此时返回一个服务器内部错误，并将异常信息直接作为响应体
                // 业务开发人员需要决定抛出什么异常，这里会直接将异常日志信息返回给客户端
                // 使用全局异常处理器捕获这个异常，并构造成响应体
                const std::string body = GlobalExceptionHandler::process(e);
                build_json_error_response(myhttp::HttpCode::INTERNAL_ERROR, body);
            }
            break;
        default:
            // 不应该到达这里
            LOG_ERROR("这是一条不可能被执行到的语句，但是被执行到了，说明RouteStatus处理有问题");
            break;
    }
}


std::string_view HttpConnection::trim(std::string_view content) {
    // 去掉前缀的空白字符
    while(!content.empty() && std::isspace(content.front())) {
        content.remove_prefix(1);
    }
    // 去掉后缀的空白字符
    while(!content.empty() && std::isspace(content.back())) {
        content.remove_suffix(1);
    }

    return content;
}

// 辅助：根据文件扩展名获取MIME类型
static const std::map<std::string_view, std::string_view> MIME_TYPES = {
    // HTML / Text
    {".html",  "text/html"},
    {".htm",   "text/html"},
    {".txt",   "text/plain"},
    {".css",   "text/css"},

    // JavaScript / JSON / XML
    {".js",    "application/javascript"},
    {".mjs",   "application/javascript"},
    {".json",  "application/json"},
    {".xml",   "application/xml"},
    {".xml",   "text/xml"},                 // 两种都常用

    // Images
    {".jpg",   "image/jpeg"},
    {".jpeg",  "image/jpeg"},
    {".png",   "image/png"},
    {".gif",   "image/gif"},
    {".webp",  "image/webp"},
    {".svg",   "image/svg+xml"},
    {".bmp",   "image/bmp"},
    {".ico",   "image/x-icon"},
    {".tiff",  "image/tiff"},
    {".avif",  "image/avif"},

    // Audio
    {".mp3",   "audio/mpeg"},
    {".wav",   "audio/wav"},
    {".ogg",   "audio/ogg"},
    {".oga",   "audio/ogg"},
    {".flac",  "audio/flac"},
    {".aac",   "audio/aac"},
    {".m4a",   "audio/mp4"},

    // Video
    {".mp4",   "video/mp4"},
    {".webm",  "video/webm"},
    {".ogv",   "video/ogg"},
    {".mpeg",  "video/mpeg"},
    {".avi",   "video/x-msvideo"},
    {".mov",   "video/quicktime"},

    // Fonts
    {".woff",  "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf",   "font/ttf"},
    {".otf",   "font/otf"},
    {".eot",   "application/vnd.ms-fontobject"},

    // Documents
    {".pdf",   "application/pdf"},
    {".doc",   "application/msword"},
    {".docx",  "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".xls",   "application/vnd.ms-excel"},
    {".xlsx",  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".ppt",   "application/vnd.ms-powerpoint"},
    {".pptx",  "application/vnd.openxmlformats-officedocument.presentationml.presentation"},

    // Archives
    {".zip",   "application/zip"},
    {".rar",   "application/x-rar-compressed"},
    {".7z",    "application/x-7z-compressed"},
    {".tar",   "application/x-tar"},
    {".gz",    "application/gzip"},

    // Form data
    {".form",  "application/x-www-form-urlencoded"},  // 一般不带扩展名，这里仅作参考

    // 二进制通用，或者直接交给默认值
    {".bin",   "application/octet-stream"},
    {".exe",   "application/octet-stream"},
    {".dll",   "application/octet-stream"},
    {".iso",   "application/octet-stream"},
    {".dat",   "application/octet-stream"}
};

bool HttpConnection::handle_static_request_and_response() {
    std::string uri  = m_request.uri;
    // 去掉最开始的斜杠，方便拼接
    if(uri.starts_with("/")) {
        uri = uri.substr(1);
    }
    // 安全检查：防止目录遍历攻击 (e.g., /../../../etc/passwd)
    if (uri.find("..") != std::string::npos) {
        build_static_error_response(myhttp::HttpCode::BAD_REQUEST, "Invalid file path.");
        return false;
    }

    // 如果访问根目录，则自动切换到index.html
    if(uri.empty()) {
        uri = "index.html";
    }
    // 构建实际的路径
    const std::filesystem::path file_path = m_doc_root / uri;

    struct stat file_stat{};
    if(stat(file_path.c_str(), &file_stat) < 0) {
        // 获取不到对应的文件
        build_static_error_response(myhttp::HttpCode::NOT_FOUND, "The requested file was not found.");
        return false;
    }

    // 检查文件权限
    if(!(file_stat.st_mode & S_IROTH)) {
        build_static_error_response(myhttp::HttpCode::FORBIDDEN, "You do not have permission to access this file.");
        return false;
    }

    if (S_ISDIR(file_stat.st_mode)) {
        build_static_error_response(myhttp::HttpCode::BAD_REQUEST, "Directories are not served.");
        return false;
    }

    // 打开文件，并将其mmap到内存
    int fd = open(file_path.c_str(), O_RDONLY);
    if(fd < 0) {
        build_static_error_response(myhttp::HttpCode::INTERNAL_ERROR, "Server failed to open the file.");
        return false;
    }

    // RAII in action: mmapped_file的析构函数会自动调用munmap
    m_response.mmapped_file.size = file_stat.st_size;
    m_response.mmapped_file.addr = mmap(nullptr, file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); // fd可以立即关闭

    if (m_response.mmapped_file.addr == MAP_FAILED) {
        // 清理 mmapped_file 状态，防止析构函数出错
        m_response.mmapped_file.addr = nullptr;
        build_static_error_response(myhttp::HttpCode::INTERNAL_ERROR, "Server failed to map the file to memory.");
        return false;
    }

    // 设置成功响应的状态码和头部
    m_response.status_code = myhttp::HttpCode::OK;
    m_response.headers["Content-Length"] = std::to_string(file_stat.st_size);
    m_response.headers["Connection"] = "keep-alive";

    // 根据扩展名决定MIME类型
    if(std::string ext = file_path.extension().string(); MIME_TYPES.contains(ext)) {
        m_response.headers["Content-Type"] = MIME_TYPES.at(ext);
    } else {
        m_response.headers["Content-Type"] = "application/octet-stream";
    }

    return true;
}

void HttpConnection::build_static_error_response(myhttp::HttpCode code, std::string_view message) {
    // 1. 设置状态码
    m_response.status_code = code;

    // 2. 构建HTML响应体
    m_response.body = std::format(
        "<html><head><title>Error</title></head><body><h1>{} {}</h1><p>{}</p></body></html>",
        static_cast<int>(code), myhttp::HttpCodeExplanations.at(code), message
    );

    // 3. 设置头部
    m_response.headers["Content-Type"] = "text/html";
    m_response.headers["Content-Length"] = std::to_string(m_response.body.size());
    m_response.headers["Connection"] = "close"; // 出错时通常关闭连接
}

void HttpConnection::build_json_error_response(myhttp::HttpCode code, std::string_view data) {
    // 1. 设置状态码
    m_response.status_code = code;

    // 2. 构建json响应体，直接从data复制过来
    m_response.body = data;

    // 3. 设置头部
    m_response.headers["Content-Type"] = "application/json";
    m_response.headers["Content-Length"] = std::to_string(m_response.body.size());
    m_response.headers["Connection"] = "close"; // 出错时通常关闭连接
}

void HttpConnection::reset() {
    m_read_buffer.clear();
    m_write_buffer.clear();
    m_parse_state = ParseState::REQUEST_LINE;
    m_request = {};     // C++11 aggregate initialization to reset the struct
    m_response = {};    // This will also trigger ~MmappedFile, calling munmap
    m_bytes_to_send = 0;
    m_bytes_have_sent = 0;
    m_connection_state = State::READING; // 关键：状态机回到起点
}


// 判断当前连接是否应保持
bool HttpConnection::is_keep_alive() const {
    // 检查HTTP/1.1的默认行为或显式的 "Connection" 头部
    if (m_request.headers.contains("connection")) {
        const auto& conn_header = m_request.headers.at("connection");
        // 为了健壮性，忽略大小写比较
        return conn_header == "keep-alive";
    }
    // 对于HTTP/1.1，默认是keep-alive
    if (m_request.version == "HTTP/1.1") {
        return true;
    } else {
        // 协议不支持
        LOG_ERROR("协议不匹配 {} != HTTP/1.1", m_request.version);
    }
    return false;
}

