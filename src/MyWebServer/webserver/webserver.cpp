//
// Created by user on 2025/11/11.
//

#include "webserver.h"

#include <cassert>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "log/logger.h"

/// 它对应了原始代码中的如下部分
/// 1. WebServer(): 默认构造函数，只分配了users和users_timer数组的内存。
/// 2. init(...): 接收一大堆参数来设置成员变量。
/// 3. log_write(): 初始化日志系统。
/// 4. sql_pool(): 初始化数据库连接池。
/// 5. trig_mode(): 根据一个整数设置具体的触发模式。
/// 6. thread_pool(): 创建线程池。
/// 7. eventListen(): 这是最重要的一个，它完成了所有的网络和epoll设置，包括：
///     1. 创建监听socket(socket, bind, listen)。
///     2. 创建epoll实例 (epoll_create)。
///     3. 将listenfd添加到epoll。
///     4. 创建信号处理管道(socketpair)并将其添加到epoll。
///     5. 设置信号处理器 (addsig)。
WebServer::WebServer(ServerConfig config):
    m_config(std::move(config)), // 获取配置信息
    m_thread_pool(m_config.thread_num, m_config.max_requests), // 初始化线程池，对应thread_pool()
    // m_sql_pool(mysql_conn_pool::get_instance()), // 获取数据库连接池单例的引用
    m_stop_server(false) // 初始化停止标志
{
    // 初始化日志系统，对应log_write
    if(!m_config.close_log) {
        // do somethings
    }

    // // 初始化数据库连接池，对应sql_conn()
    // m_sql_pool.init(m_config.db_url, m_config.db_user, m_config.db_password, m_config.db_name, m_config.db_port, m_config.sql_conn_num);

    // 实现eventListen()，它有三部分组成：配置监听socket、配置epoll、配置信号处理器
    try {
        setup_listen_socket();
        setup_epoll_and_signals();
    } catch (const std::runtime_error &e) {
        // 记录日志，重新抛出
        throw;
    }
    // 记录日志
}


WebServer::~WebServer() {
    // RAII负责大部分清理工作
    if(m_listen_fd != -1) close(m_listen_fd);
    if(m_epoll_fd != -1) close(m_epoll_fd);
    if(m_pipe_fd[0] != -1) close(m_pipe_fd[0]);
    if(m_pipe_fd[1] != -1) close(m_pipe_fd[1]);
}

/// 公共入口函数：启动服务器
void WebServer::run() {
    // 日志
    LOG_INFO("========== Server starting ==========");
    // 启动定时器（如果需要，例如周期性的SIGALRM）
    // 在我们的新设计中，我们会在event_loop中处理，所以这里可以为空

    event_loop(); // 进入主事件循环

    LOG_INFO("========== Server stopping ==========");
}

void WebServer::get(const std::string& url, myhttp::ApiHandler handler) {
    m_router.get(url, std::move(handler));
}

void WebServer::post(const std::string &url, myhttp::ApiHandler handler) {
    m_router.post(url, std::move(handler));
}


/// 完成监听socket的初始化
/// 对应原始的eventListen的前半部分
void WebServer::setup_listen_socket() {
    // 创建listen socket
    m_listen_fd = socket(PF_INET, SOCK_STREAM, 0);
    if(m_listen_fd < 0) {
        throw std::runtime_error("Failed to create listen socket");
    }

    // 配置选项

    // 设置关闭选项，优雅关闭连接
    // fixme 这里用到了SO_LINGER选项，但是实际上没有用？
    // 1. 默认行为 (l_onoff = 0) 这是最常见的默认设置。
    //      当调用 close() 时，函数会立即返回。
    //      TCP协议栈会接管后续的数据发送任务，尝试将输出缓冲区中剩余的数据发送给对端。
    //      这个过程被称为“优雅关闭”（graceful close），是后台异步进行的。应用程序无需等待数据发送完成。
    // 2. 优雅关闭并等待 (l_onoff != 0, l_linger > 0)
    //      当调用 close() 时，进程会被阻塞。
    //      TCP协议栈会发送缓冲区中的剩余数据。
    //      进程会一直等待，直到数据全部发送成功并收到对端的ACK确认，或者等待超时（l_linger指定的时间）。
    //      如果在超时时间内成功发送并收到确认，close() 返回0。
    //      如果超时，close() 返回-1，并设置 errno 为 EWOULDBLOCK。这可以确保数据被可靠地发送，或者让应用程序知道在规定时间内没有发送成功。
    // 3. 强制关闭 (l_onoff != 0, l_linger = 0)
    //      当调用 close() 时，TCP连接会立即被终止。
    //      输出缓冲区中任何尚未发送的数据都会被丢弃。
    //      系统会向对端发送一个RST（Reset）报文，而不是正常的FIN报文。
    //      对端收到RST后，会认为连接异常中断。这种方式被称为“强制关闭”或“硬关闭”（hard close）。
    // 监听socket被设置SO_LINGER实际上是无效行为，以为：
    // 1. 监听socket **没有数据可发送**，它通过accept返回一个新的已连接socket后，所有数据交换都交给该已连接socket完成。它的**输出缓冲区始终为空**
    // 2. 对监听socket执行close()仅仅是停止该端口并释放相关资源。它不涉及TCP四次挥手的过程，因为没有一个具体的TCP连接与他关联
    if(m_config.opt_linger) {
        // l_onoff=1, l_linger=1则表示
        struct linger tmp = {1, 1};
        setsockopt(m_listen_fd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    } else {
        struct linger tmp = {0, 1};
        setsockopt(m_listen_fd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    // 在TCP四次挥手阶段，主动方接收到被动方的FIN后（最后一次回收），它需要发送一个ACK报文作为确认。
    // 发送完毕后，主动方进入TIME_WAIT阶段（等待2*MSL），而被动方接受到这个ACK之后立即进入CLOSED状态
    // 然而，如果主动方发送的最后一个ACK丢失，被动方就需要重传FIN，TIME_WAIT阶段允许这种重试（否则会导致对端异常终止）
    // TIME_WAIT阶段的存在也能够防止旧的连接干扰新的连接
    // 而为socket设置SO_REUSEADDR选项，则是：允许socket绑定到一个已经处于TIME_WAIT状态的地址和端口，作用是：
    // 1. 解决服务器短时间内重启时，因为端口被占用（上一次关闭时处于TIME_WAIT状态并持续1分钟）（**这里的主要原因**）
    // 2. 允许多个实例绑定到不同IP的同一个端口：在拥有多个IP地址的服务器上，如果一个服务进程绑定了 0.0.0.0:8080（通配地址，监听所有IP），那么在默认情况下，其他进程无法再绑定到 192.168.1.100:8080 (一个具体的IP)。设置 SO_REUSEADDR 后，则允许这种绑定
    // 3. UDP多播时也需要：允许多个进程绑定到同一个IP地址和端口，以便它们都能接收到发送到该多播组的数据
    int flag = 1;
    setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    // 初始化连接结构体
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_config.port);

    // 绑定到本地端口
    if(bind(m_listen_fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0) {
        throw std::runtime_error("Failed to bind listen socket");
    }

    // listen
    if(listen(m_listen_fd, 5) < 0) {
        throw std::runtime_error("Failed to listen on socket");
    }
}

/// 为epoll初始化，并配置信号处理函数
/// 对应之前的eventListen的后半部分
void WebServer::setup_epoll_and_signals() {
    // 创建epoll实例
    m_epoll_fd = epoll_create(5);
    if(m_epoll_fd == -1) {
        throw std::runtime_error("Failed to create epoll instance");
    }

    // 将监听socket加入epoll
    epoll_event event;
    event.data.fd = m_listen_fd;
    // note 监听线程不需要注册EPOLLONESHOT，因为自始至终都只有它一个线程在处理连接，不存在鬓发问题
    event.events = EPOLLIN | EPOLLRDHUP; // 监听可读事件，以及连接被对方关闭的事件
    // ET触发模式如果开启，则使用ET触发模式
    if(m_config.listen_trig_mode == TriggerMode::ET) {
        event.events |= EPOLLET;
    }
    // 通过epoll_ctl对内核的中的事件表进行修改
    if(epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_listen_fd, &event) == -1) {
        throw std::runtime_error("Failed to add listen_fd to epoll");
    }

    // 监听socket设置为非阻塞
    // note 对于ET模式，一定要开启非阻塞，否则因为读或写没有后续事件而导致无法返回，进而饥饿，但是LT也需要开启非阻塞？答案是也需要
    set_nonblocking(m_listen_fd);

    // 创建信号管道
    if(socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipe_fd) == -1) {
        throw std::runtime_error("Failed to create signal pipe");
    }

    // 将管道读端加入epoll
    set_nonblocking(m_pipe_fd[0]);
    event.data.fd = m_pipe_fd[0];
    event.events = EPOLLIN | EPOLLET; // 信号常用ET模式，原始版本的代码使用的是LT模式
    if(epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_pipe_fd[0], &event) == -1) {
        throw std::runtime_error("Failed to add pipe_fd[0] to epoll");
    }

    // 设置信号处理器
    // 将管道的写端存入静态变量，供静态回调函数使用
    s_signal_pipe_write_fd = m_pipe_fd[1];

    // 处理信号
    add_signal(SIGPIPE, SIG_IGN); // 忽略原始管道信号
    add_signal(SIGTERM, signal_handler_callback); // 终止信号
    add_signal(SIGINT, signal_handler_callback);  // Ctrl+C
}

// 私有主循环函数：实现原始 eventLoop 的功能
void WebServer::event_loop() {
    epoll_event events[MAX_EVENTS];

    while (!m_stop_server) {
        // note 这里的实现与《linux高性能服务器编程》的区别在于，超时时间是固定的；原版会根据等待了多长时间动态变化，但是需要将Timer容器改为堆等具有优先级的容器
        constexpr int timeout = 100;
        // epoll_wait: 等待事件发生
        // 使用一个100ms的超时，而不是无限等待(-1)。
        // 这样做的好处是：
        // 1. 即使没有IO事件，循环也能周期性地继续，可以处理非IO任务（如定时器检查）。
        // 2. 使得服务器关闭更加平滑，因为它会定期检查 m_stop_server 标志。
        int event_count = epoll_wait(m_epoll_fd, events, MAX_EVENTS, timeout);

        if (event_count < 0 && errno != EINTR) {
            std::cerr << "epoll_wait failure" << std::endl;
            break;
        }

        // 遍历所有就绪的事件
        for (int i = 0; i < event_count; ++i) {
            int sockfd = events[i].data.fd;
            uint32_t triggered_events = events[i].events;

            if (sockfd == m_listen_fd) {
                // 1. 处理新的客户端连接
                handle_new_connection();
            }
            else if (sockfd == m_pipe_fd[0]) {
                // fixme 万一不是可读事件呢？
                // 2. 处理信号
                if(triggered_events & EPOLLIN) {
                    handle_signal();
                } else {
                    // 会报错？
                    LOG_ERROR("意外的信号，不是可读事件");
                }
            }
            else {
                // 3. 处理已连接客户端的事件（读、写、关闭）
                handle_connection_event(sockfd, triggered_events);
            }
        }

        // 每次循环结束后，检查并处理到期的定时器
        // 这取代了原始代码中基于SIGALRM和timeout标志的机制，更简单、更可靠。
        m_timer_manager.tick();
    }
}


int WebServer::set_nonblocking(int fd) {
    // 该实现直接从 Utils 迁移，完全正确
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}


void WebServer::add_signal(int sig, void(handler)(int), bool restart) {
    // 该实现直接从 Utils 迁移，完全正确
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart) sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 重置EPOLLONESHOT事件
void WebServer::modfd(int sockfd, uint32_t events) const {
    // ===================================================================
    // 新增的 modfd 实现
    // ===================================================================
    epoll_event event;
    event.data.fd = sockfd;
    if(m_config.conn_trig_mode == TriggerMode::ET) {
        // 统一加上ET, ONESHOT, RDHUP
        event.events = events | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    } else {
        // LT模式则只需要后二者
        event.events = events | EPOLLONESHOT | EPOLLRDHUP;
    }
    epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, sockfd, &event);
}

// 监听socket上有事件时被调用，用于处理新的连接
void WebServer::handle_new_connection() {
    LOG_DEBUG("处理连接请求");
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);

    // 根据监听socket的模式（LT或ET）循环accept
    do {
        int connfd = accept(m_listen_fd, (struct sockaddr *)&client_address, &client_addrlength);

        if (connfd < 0) {
            // 对于ET模式，EAGAIN或EWOULDBLOCK表示所有连接都已处理完毕
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            std::cerr << "accept error: " << strerror(errno) << std::endl;
            // 日志
            LOG_ERROR("accept error, 报错信息为: {}", strerror(errno));
            return;
        }

        // todo 如果连接数达到上限（虽然我们用map移除了硬上限，但可以设置一个软上限）
        // if (m_connections.size() >= MAX_CONNECTIONS) { ... }

        std::cout << "New connection from fd: " << connfd << std::endl;

        // 1. 将新连接设置为非阻塞，这是ET必须要求的（LT模式最好设置）
        set_nonblocking(connfd);

        // 2. 创建HttpConnection对象并存入map
        auto epoll_modifier = [this](int fd, uint32_t ev) {
            if(this->m_connections.contains(fd)) {
                this->modfd(fd, ev);
            }
        };
        // m_connections[connfd] = std::make_shared<HttpConnection>(connfd, client_address, m_config.m_root, m_sql_pool, m_router, epoll_modifier);
        m_connections[connfd] = std::make_shared<HttpConnection>(connfd, client_address, m_config.m_root, m_router, epoll_modifier);

        // 3. 将新连接的fd注册到epoll
        epoll_event event;
        event.data.fd = connfd;
        // note 这里必须要使用ONESHOT，当一个 EPOLLONESHOT 事件被epoll_wait返回后，内核会自动禁用对这个socket的任何后续事件监听。即使数据再次到达，也不会再触发。你必须通过 epoll_ctl(MOD) 手动重新激活它，它正是为了解决Reactor模式下的竞争条件而生的。如果没有ONESHOT：
        // 线程A（工作线程）正在处理sockfd的任务。
        // 此时，客户端又发送了新数据，epoll因为没有ONESHOT而再次触发了EPOLLIN事件。
        // 主线程响应这个新事件，将同一个sockfd的第二个任务交给了线程B。
        // 现在，线程A和线程B正在并发地操作同一个HttpConnection对象，访问它的读写缓冲区、解析状态等。这是灾难性的数据竞争，会导致程序崩溃或数据损坏。
        event.events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT; // 监听读事件和连接关闭事件（将来会被m_connections[connfd]所对应的HttpConnection处理）
        if (m_config.conn_trig_mode == TriggerMode::ET) {
            event.events |= EPOLLET;
        }
        epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, connfd, &event);

        // 4. 为新连接设置定时器
        m_timer_manager.add_timer(connfd, m_config.connection_timeout, [this, connfd]() {
            LOG_ERROR("连接超时，自动关闭：{}", connfd);
            this->close_connection(connfd);
        });

    } while (m_config.listen_trig_mode == TriggerMode::ET);
}


void WebServer::handle_connection_event(int sockfd, uint32_t events) {
    LOG_INFO("处理客户端事件，socket描述符={}，事件类型={}", sockfd, events);
    auto it = m_connections.find(sockfd);
    if (it == m_connections.end()) {
        // 连接可能已经被定时器关闭了
        return;
    }
    auto conn_ptr = it->second;

    // 处理连接关闭或错误事件
    // EPOLLRDHUP: 对端关闭连接或写半部
    // EPOLLHUP: 连接被挂起
    // EPOLLERR: 发生错误
    if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        LOG_WARN("socket {} 对应的连接关闭，被挂起，或者发生错误", sockfd);
        close_connection(sockfd);
        return;
    }

    // note 到此说明连接上有读写活动，刷新其定时器，延长生命周期
    m_timer_manager.adjust_timer(sockfd, m_config.connection_timeout);


    // 统一处理读写事件
    // fixme 这里直接将任务交给线程池，实际上属于Reactor模式；而非Proactor模式
    LOG_INFO("处理客户端事件，将任务加入线程池");
    bool isET = m_config.conn_trig_mode == TriggerMode::ET;
    m_thread_pool.enqueue([conn_ptr, isET]() {
       conn_ptr->process(isET);
    });
}

void WebServer::handle_signal() {
    char signals[1024];
    // 由于我们在setup_epoll_and_signals()中将信号处理（管道的读端）设置为了ET模式，必须一次性读完
    int ret = recv(m_pipe_fd[0], signals, sizeof(signals), 0);
    if (ret <= 0) {
        return;
    }

    for (int i = 0; i < ret; ++i) {
        switch (signals[i]) {
            case SIGTERM:
            case SIGINT:
                std::cout << "Termination signal received, shutting down." << std::endl;
            m_stop_server = true; // 设置停止标志
            break;
            // 注意：SIGALRM信号不再需要在这里处理，因为定时器逻辑已改变
            default:
                break;
        }
    }
}

/// 静态信号处理回调
/// 向管道写入信号值，之后，主事件循环的epoll_wait将会被唤醒，并且管道的读端就有可读事件
void WebServer::signal_handler_callback(int sig) {
    // 这是self-pipe技巧的核心
    int msg = sig;
    // 使用 s_signal_pipe_write_fd 而不是全局变量
    send(s_signal_pipe_write_fd, (char *)&msg, 1, 0);
}

void WebServer::close_connection(int sockfd) {
    // 1. 从epoll中移除，这是第一步，确保不再有任何事件被触发
    //    即使有工作线程正在处理，处理完后也无法重新注册事件
    epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, sockfd, nullptr);

    // 2. 查找并从map中移除连接对象
    const auto it = m_connections.find(sockfd);
    if (it == m_connections.end()) {
        // 如果map中没有，可能已经被其他原因关闭，直接关闭fd即可
        close(sockfd);
        m_timer_manager.remove_timer(sockfd);
        return;
    }

    const auto conn_to_close = it->second;
    m_connections.erase(it);

    // 3. 通知HttpConnection对象它被关闭了
    //    这会设置其内部的 m_is_closed 标志。
    //    如果此时有工作线程在处理它，这个标志可以防止它做危险的操作。
    conn_to_close->close_connection();

    // 4. 关闭socket文件描述符
    // SOKET清理的核心在于：close(sockfd) 这个系统调用。它是一个强大的“清理”命令，它告诉内核：
    // “我不再关心这个socket了。”，实际上开始四次挥手的前两个阶段：服务器端关闭
    // “请向对端发送FIN，开始关闭流程。”，调用close()会向客户端发送一个FIN包，表示（服务器）这边已经没有数据要发送了
    // “如果接收缓冲区里还有未读的数据，请全部丢弃，并发送RST作为警告。”
    // 也不需要担心客户端不规范的行为——即在对端close之后继续write：当客户端尝试在一个已经被服务器RST的连接上write时，它的write调用会失败，通常返回ECONNRESET (Connection reset by peer) 错误
    close(sockfd);

    // 5. 移除定时器
    // note 一定要移除定时器，否则会因为定时器到期而close_connection被再次调用，导致close(sockfd)被执行两次
    m_timer_manager.remove_timer(sockfd);

    // todo http_conn::m_user_count--还没有处理

    // 当 conn_to_close 这个 shared_ptr 离开作用域时，
    // 如果没有其他地方引用它（在我们的设计中是没有的），
    // HttpConnection 对象的析构函数将被自动调用，完成内部资源的清理。
}

