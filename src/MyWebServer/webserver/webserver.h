//
// Created by user on 2025/11/11.
//

#ifndef WERBSERVER_H
#define WERBSERVER_H



#include <sys/epoll.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <atomic>

#include "../base/ThreadPool.h"       // 你实现的线程池
#include "../mysql_connection_pool/mysql_connection_pool.h"  // 你实现的数据库连接池
#include "../http/http_conn.h"   // 模拟的HTTP连接类
#include "../utils/timer.h"            // 模拟的定时器管理器

// 使用 enum class 增强类型安全，相较于enum，它具有单独的作用域，避免意外赋值和magic numbers
enum class TriggerMode { LT, ET };
enum class ActorModel { REACTOR, PROACTOR };

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENTS = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位

// 使用配置结构体，使构造更清晰
struct ServerConfig {
    std::string m_root; // 根目录
    int port = 9006;
    TriggerMode listen_trig_mode = TriggerMode::ET;
    TriggerMode conn_trig_mode = TriggerMode::ET;
    bool opt_linger = false;

    // 数据库配置
    std::string db_url = "127.0.0.1";
    int db_port = 3306;
    std::string db_user;
    std::string db_password;
    std::string db_name;
    int sql_conn_num = 8;

    // 线程池配置
    int thread_num = 8;
    int max_requests = 10000;

    // 日志配置
    bool close_log = false;

    // 并发模型
    ActorModel actor_model = ActorModel::PROACTOR;

    // socket连接的超时时间
    std::chrono::seconds connection_timeout {60};
};

class WebServer {
public:
    // 构造函数接收一个配置对象，而不是一长串参数
    explicit WebServer(ServerConfig config);

    // 析构函数现在非常简洁，RAII会自动处理所有资源的释放
    ~WebServer();

    /// 启动服务器的主循环
    void run();

    /// 新增处理GET请求的handler，用于用户自定义处理
    void get(const std::string& url, myhttp::ApiHandler handler);

    /// 新增处理POST请求的handler，用于用户自定义处理
    void post(const std::string& url, myhttp::ApiHandler handler);

private:
    // --- 私有辅助函数 ---
    void setup_listen_socket();
    void setup_epoll_and_signals();
    void event_loop();

    // 从Utils迁移过来的辅助功能
    static int set_nonblocking(int fd);
    void add_signal(int sig, void(handler)(int), bool restart = true);

    void modfd(int sockfd, uint32_t events) const;

    // 事件处理函数
    /// 监听socket上有事件时被调用，用于处理新的连接，对应原始代码中的dealclientdata
    void handle_new_connection();
    /// 已连接socket上有事件时被调用，用于处理实际的IO，对应原始代码中的dealwithread()、dealwithwrite()和关闭逻辑
    void handle_connection_event(int sockfd, uint32_t events);
    /// 处理信号，对应原始代码中的dealwithsignal
    void handle_signal();

    static void signal_handler_callback(int sig);

    /// 关闭连接
    /// 该函数会：
    /// 从epoll中移除对应的socket fd
    /// 连接map中删除对应的连接
    /// 移除对应的定时器
    void close_connection(int sockfd);

private:
    // --- 核心组件 (通过值或引用持有，不再是裸指针) ---
    ServerConfig m_config;                      // 存储所有配置
    ThreadPool m_thread_pool;                   // 线程池对象 (值成员)
    // mysql_conn_pool& m_sql_pool;                // 数据库连接池 (单例引用)
    TimerManager m_timer_manager;               // 定时器管理器 (值成员)

    // --- 网络和事件处理相关 ---
    int m_listen_fd = -1;                       // 监听Socket文件描述符
    int m_epoll_fd = -1;                        // Epoll实例文件描述符
    int m_pipe_fd[2] = {-1, -1};                // 用于信号处理的管道

    // 专门用于在静态信号处理器和WebServer服务实例之间通信
    // note 在c++17之前，（非const修饰的）静态字段必须在.h头文件中被声明（如下），在.cpp文件中被定义（初始化，即int WebServer::s_signal_pipe_write_fd = -1），在头文件中的只是声明
    // static int s_signal_pipe_write_fd;
    // 而且不能再头文件中为其初始化，例如在头文件中写int WebServer::s_signal_pipe_write_fd = 1;，虽然编译不会报错，但是链接时，如果这个头文件被include多次，就会出现重复定义的链接错误，违背了“一次定义规则”
    // 从c++17开始，允许在static前增加inline关键字，这时就能直接在头文件中初始化
    // 类的定义中写只是声明它的存在，但并没有分配任何内存。定义实际上是分配内存。
    // 而inline关键字则是告诉链接器：这个变量可能会在多个翻译单元中出现定义，但没关系，只需要在最终的可执行文件中为它保留一份内存，并将其他所有对它的引用都指向这个同一个实例即可
    inline static int s_signal_pipe_write_fd;

    // 使用 std::unordered_map 管理所有客户端连接，替代原来的数组
    // Key:   socket文件描述符 (int)
    // Value: 指向HttpConnection对象的独占智能指针
    std::unordered_map<int, std::shared_ptr<HttpConnection>> m_connections;

    /// 定义一个Router成员，将来会被传入HttpConnection进行处理
    /// 直接使用默认构造函数
    Router m_router {};

    // 用于停止服务器主循环的原子标志
    std::atomic<bool> m_stop_server;
};




#endif //WERBSERVER_H
