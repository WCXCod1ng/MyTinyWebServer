//
// Created by user on 2025/11/11.
//

#ifndef MYSQL_CONNECTION_POOL_H
#define MYSQL_CONNECTION_POOL_H
#include <list>
#include <memory>
#include <semaphore>
#include <mysql/mysql.h>

/// 所实现的连接池没有考虑如下功能
/// 1. 连接测试
/// 2. 连接附带idleTimeout属性
/// 3. 池空时的策略是继续等待还是直接返回错误；支持带超时时间的等待


// --- 前向声明 ---
// 告诉编译器，稍后会有一个名为 connection_pool 的类。
// 这是为了解决 ConnectionPoolDeleter 需要 connection_pool* 指针，
// 而 connection_pool 又需要 ConnectionPoolDeleter 作为友元的循环依赖问题。
class mysql_conn_pool;

// --- 自定义删除器 ---
// 当 std::unique_ptr 的默认行为（在销毁时对管理的指针调用 delete）不符合我们的需求时，我们可以通过提供自定义删除器 (Custom Deleter) 来重载这一行为。
// 自定义删除器本质上是一个可调用对象（函数、函数指针、Lambda表达式或带有operator()的类/结构体），它接收一个裸指针作为参数，并定义如何“释放”这个指针代表的资源。有三种方式
// 1. 使用函数指针作为unique_ptr的第二个模板参数，这样的删除器是无状态的，
// 2. 使用lambda表达式作为unique_ptr的第二个模板参数
// 3. 使用函数对象（重载了operator()的类或结构体），也就是这里所使用的
struct MySQLConnectionPoolDeleter {
    // 当 unique_ptr 被销毁时，这个 operator() 会被自动调用。
    void operator()(MYSQL* conn) const;

    // 这个指针指向连接的来源地——连接池实例。
    mysql_conn_pool* pool = nullptr;
};

// --- RAII 连接包装器 ---
// 这是暴露给用户的最终类型，一个“作用域连接”。
// 它是一个智能指针，当它离开作用域时，会自动通过 ConnectionPoolDeleter 将连接归还。
// 它被认为是：MySQL连接的封装，而且还包含了一个指向它所在池的指针，并实现了该连接离开作用域时的动作（归还连接）
using ScopedConnection = std::unique_ptr<MYSQL, MySQLConnectionPoolDeleter>;

class mysql_conn_pool {
public:
    // 单例模式与生命周期管理
    // 禁止拷贝和赋值，以实现单例模式
    mysql_conn_pool(const mysql_conn_pool&) = delete;
    mysql_conn_pool& operator=(const mysql_conn_pool&) = delete;

    // 获取连接池实例，将其定义为静态方法，这也是单例模式的实现。
    // 根据单例模式的要求，调用方不应当占有所有权，因此首先不能使用unique_ptr等智能指针（它代表了占有所有权），其次返回一个值更是不行（因为单例模式要求不能有其他实例），所以可以返回的类型有裸指针或引用，我们这里返回引用是更符合现代C++的特点，其优点是：1.绝对不会为空；2. 可以防止调用者误使用delete而导致二次析构
    static mysql_conn_pool& get_instance();

    // 初始化连接池，创建所有的物理连接
    void init(const std::string& url, const std::string& user, const std::string& password, const std::string& dbname, int port, int max_conn);

    // 核心功能
    // 获取连接
    // 现在我们可以看到 get_connection 和 release_connection 是如何构成一个完美的闭环的：
    // 初始状态: m_conns 队列中有 N 个连接，m_semaphore 计数为 N。
    // get_connection 被调用:
    // m_reserve.acquire() -> 信号量计数变为 N-1。
    // 从 m_conns 队列中取出一个连接 -> 队列大小变为 N-1。
    // 连接被使用: 线程持有从 get_connection 返回的 ScopedConnection 对象。
    // ScopedConnection 销毁:
    // 其删除器调用 release_connection。
    // release_connection 被执行:
    // 将连接放回 m_conns 队列 -> 队列大小变回 N。
    // m_reserve.release() -> 信号量计数变回 N。
    ScopedConnection get_connection();

    // 注意，这里没有release链接，因为链接的生命周期交给了unique_ptr来管理，超出作用域时就会自动失效

    // 销毁所有连接
    void destroy_pool();

private:
    // 默认构造函数设置为私有
    mysql_conn_pool();
    // 析构函数也设置为私有，也是为了防止外界通过delete删除
    ~mysql_conn_pool();

    // 讲一个连接归还到池中。该函数不会被外界使用，而是有ConnectionPoolDeleter自动调用
    void release_connection(MYSQL* conn);

    // 数据库信息
    std::string m_url; // 数据库地址
    std::string m_user; // 用户名
    std::string m_password; // 密码
    std::string m_dbname; // 数据库名称
    int m_port = 0; // 端口号

    // 连接池信息
    int m_max_conn = 0; // 最大连接数量
    int m_cur_conn = 0; // 当前已使用的连接数
    int m_free_conn = 0; // 当前空闲的连接数
    std::mutex m_mutex; // 互斥锁
    std::counting_semaphore<> m_reserve; // 信号量默认被初始化为0，将来会在init函数中被初始化为最大连接个数

    // 存储空闲数据库连接的队列，这是我们实际需要管理的资源（我们这里使用list来作为队列，原因在于它是离散的，没有空间顾虑）
    std::list<MYSQL*> m_conns;

    // 允许MySQLConnectionPoolDeleter访问本类的私有成员
    friend struct MySQLConnectionPoolDeleter;
};



#endif //MYSQL_CONNECTION_POOL_H
