//
// Created by user on 2025/11/11.
//

#include "mysql_connection_pool.h"
#include <cassert>

void MySQLConnectionPoolDeleter::operator()(MYSQL *conn) const {
    // 该连接被销毁时，直接调用连接池提供的release_connection方法

    // 安全性检查：确保有一个有效的连接和一个有效的连接池指针，这个连接池指针指向的就是分配它的连接池。
    if(conn && pool) {
        pool->release_connection(conn);
    }
}


mysql_conn_pool::mysql_conn_pool() : m_reserve(0) {
    // 构造函数体可以是空的，因为所有工作都在初始化列表中完成了
}


mysql_conn_pool& mysql_conn_pool::get_instance() {
    // 这行代码声明了一个 static 类型的局部变量 instance。在 C++ 中，当 static 关键字用于函数内部的局部变量时，它具有两个至关重要的特性：
    // 生命周期(Lifetime)：这个变量的生命周期不是函数作用域。 它不会在函数返回时被销毁。相反，它在第一次执行到这行声明时被创建和初始化，并且会一直存活到整个程序结束。 这完美地满足了单例对象需要全局存活的要求。
    // 初始化(Initialization)：instance 对象只会被初始化一次。当 GetInstance() 函数第一次被调用时，程序会执行这行代码，构造 connection_pool 对象。 在后续所有对 GetInstance()的调用中，程序会跳过这行声明和初始化，直接使用已经存在的 instance 对象。这就保证了 connection_pool 只有一个实例。

    // 在多线程环境中，如果两个或多个线程同时首次调用 GetInstance()，会发生什么？会不会创建出多个实例？
    // C++11 标准的解决方案：
    // C++11 标准明确规定：“如果一个静态局部变量的初始化过程被多个线程同时进入，那么这个初始化过程只会被一个线程执行，其他线程必须等待该初始化过程完成。”这意味着 C++ 编译器和运行时库会自动在这行 static connection_pool instance; 的初始化周围生成线程安全的保护代码（效果类似于一个内部的互斥锁和 std::call_once
    static mysql_conn_pool instance;

    return instance;
}

void mysql_conn_pool::init(const std::string &url, const std::string &user, const std::string &password, const std::string &dbname, int port, int max_conn) {
    m_url = url;
    m_user = user;
    m_password = password;
    m_dbname = dbname;
    m_port = port;
    m_max_conn = max_conn;

    // 在创建之前，加锁以保证线程安全
    std::lock_guard<std::mutex> lock(m_mutex);

    // 循环创建max_conn个数量的数据库连接
    for(int i = 0; i < m_max_conn; ++i) {
        MYSQL * conn = mysql_init(nullptr);

        // 检查是否初始化成功
        if(!conn) {
            throw std::runtime_error("MySQL Error: mysql_init() failed.");
        }

        // 尝试建立真正的物理连接
        conn = mysql_real_connect(conn, m_url.c_str(), m_user.c_str(), m_password.c_str(), m_dbname.c_str(), m_port, nullptr, 0);

        // 检查连接是否成功
        if(!conn) {
            // 如果连接失败，获取具体的错误信息，然后抛出异常
            // 注意：根据MySQL C API，如果 mysql_real_connect 失败，它不会自动释放
            // 由 mysql_init 创建的句柄，我们需要手动关闭它以防内存泄漏。
            // 但为了获取错误信息，我们需要先从这个句柄中读取。
            const std::string error_msg = mysql_error(conn);
            mysql_close(conn); // 清理失败的句柄
            throw std::runtime_error("MySQL Error: mysql_real_connect() failed: " + error_msg);
        }

        // 创建成功则加入队列
        m_conns.push_back(conn);
    }

    // 更新信号量，通知有m_max_conn个资源可用
    if(m_max_conn > 0) {
        m_reserve.release(m_max_conn);
    }
} // 当函数离开作用域时，lock_guard会自动执行析构函数，并调用m_mutex.unlock()



ScopedConnection mysql_conn_pool::get_connection() {
    // 1. 等待并获取一个信号量“许可”
    // 这个操作会原子地检查信号量计数，如果大于0，则减1并立即返回。
    // 如果计数为0，当前线程将被阻塞（休眠），直到其他线程 release 信号量。
    m_reserve.acquire();

    // 上锁
    std::lock_guard<std::mutex> lock(m_mutex);

    // 健壮性检查：理论上锁后队列不应该为空
    if(m_conns.empty()) {
        // 在Debug模式下，立即断言并终止，让开发者立刻看到调用栈
        assert(!"Connection pool invariant violated: semaphore count and queue size mismatch.");

        // 在Release模式下，为了保持运行，可以抛出异常或记录日志并返回空指针
        m_reserve.release();
        throw std::logic_error("Connection pool state error..."); // 推荐
        return ScopedConnection(nullptr, MySQLConnectionPoolDeleter{this});
    }

    // 从队列的头部去除一个连接
    MYSQL * conn = m_conns.front();
    m_conns.pop_front();

    //  将裸指针包装成ScopedConnection并返回
    return ScopedConnection(conn, MySQLConnectionPoolDeleter{this});
}

mysql_conn_pool::~mysql_conn_pool() {
    this->destroy_pool();
}


void mysql_conn_pool::destroy_pool() {
    // 上锁，确保在销毁的过程中没有其他线程在操作队列
    std::lock_guard<std::mutex> lock(m_mutex);

    // 循环遍历并关闭池中所有的空闲连接
    while(!m_conns.empty()) {
        MYSQL* conn = m_conns.front();
        m_conns.pop_front();

        mysql_close(conn);
    }
} // m_reserve的析构函数会自动清理其资源，因此无需我们手动操作

// 归还连接。
// note 注意这个函数永远不会被用户直接调用。它是一个内部实现细节，由 ScopedConnection 的自定义删除器 ConnectionPoolDeleter 在其生命周期结束时自动调用
void mysql_conn_pool::release_connection(MYSQL *conn) {
    // 生产者没有限制，因为这实际上被get_connection限制过了（它限制了最多多少个连接会调用conn，所以这里可以放心的直接归还）

    // 有效性检查
    if(conn) {
        // 上锁
        std::lock_guard<std::mutex> lock(m_mutex);

        // 将连接插入队列尾部
        m_conns.push_back(conn);

        // 添加一个permit
        m_reserve.release();
    }
}

// ### 缺少的核心特性
//
// 以下是您的实现与生产级连接池之间最主要的差距：
//
// #### 1. 连接健康检查与验证 (Connection Health Checking / Validation)
//
// *   **问题是什么？**：数据库连接是一个脆弱的 TCP 长连接。它可能会因为多种原因“悄无声息地”死掉，例如：
//     *   数据库服务器重启或崩溃。
//     *   网络中间设备（防火墙、路由器）因超时而清除了会话。
//     *   数据库管理员 (DBA) 手动杀掉了这个连接。
//     此时，你的连接池队列里持有的 `MYSQL*` 句柄就成了一个“僵尸连接”，它在本地看起来有效，但任何基于它的查询都会立即失败。
//
// *   **缺少了什么？**：一个在将连接交给应用程序之前，验证其有效性的机制。
//
// *   **生产级实现**：
//     *   **借出时验证 (Test on Borrow)**：在 `GetConnection` 函数中，从队列取出连接后，会执行一条非常快速、低开销的“心跳”查询（例如 `SELECT 1`）。如果查询成功，才将连接返回给调用者；如果失败，则**丢弃**这个失效连接，并尝试获取下一个，同时可能会触发创建一个新连接来补充池。
//     *   **归还时验证 (Test on Return)**：在 `ReleaseConnection` 时执行验证。
//     *   **空闲时验证 (Test on Idle)**：连接池有一个后台线程，会周期性地检查那些长时间未被使用的空闲连接，确保它们还活着。
//
// #### 2. 连接生命周期管理 (Connection Lifecycle Management)
//
// *   **问题是什么？**：即使连接是健康的，让一个连接无限期地存活也可能带来问题，比如微小的内存泄漏累积，或者连接状态变得陈旧。
//
// *   **缺少了什么？**：对连接最大生命周期和最大空闲时间的控制。
//
// *   **生产级实现**：
//     *   **最大生命周期 (Max Lifetime)**：连接池会记录每个连接的创建时间。当一个连接的总存活时间超过设定的阈值（例如 30 分钟），即使它仍然健康，当它被归还时，连接池也会选择关闭它并创建一个全新的连接来替代。这有助于定期“刷新”所有连接，避免潜在的资源泄漏。
//     *   **最大空闲时间 (Idle Timeout)**：如果一个连接在池中空闲的时间超过了设定的阈值（例如 10 分钟），连接池会自动关闭它，并将池的大小缩减到最小连接数。这可以释放不必要的数据库资源。
//
// #### 3. 获取连接的超时机制 (Acquisition Timeout)
//
// *   **问题是什么？**：在您的实现中，如果连接池已空，`m_semaphore.acquire()` 会使线程**无限期地等待**。在高负载下，如果所有连接都被长时间占用，后续的所有请求线程都会被永久阻塞，导致整个应用看起来像“卡死”了。
//
// *   **缺少了什么？**：一个带有超时时间的获取机制。
//
// *   **生产级实现**：
//     *   `GetConnection` 会有一个可选的超时参数。例如 `GetConnection(timeout_ms)`。
//     *   内部会使用 `std::counting_semaphore::try_acquire_for()` 或 `std::condition_variable::wait_for()`。如果在指定时间内没有获得连接，函数就会停止等待，并抛出一个“获取连接超时”的异常或返回一个空对象。这使得应用程序可以优雅地处理高峰期的负载，例如向用户返回“系统繁忙，请稍后再试”的提示，而不是无限期地卡住。
//
// #### 4. 连接泄漏检测 (Leak Detection)
//
// *   **问题是什么？**：尽管我们用了 RAII，但如果程序员不小心导致 `ScopedConnection` 对象泄漏（例如，通过 `new` 创建但忘记 `delete`，或者循环引用），连接还是会永久性地丢失。
//
// *   **缺少了什么？**：一种在连接被借出太长时间后发出警告的机制。
//
// *   **生产级实现**：
//     *   当一个连接被借出时，连接池会记录下借出时间和当时的调用栈。
//     *   一个后台的“管家线程” (Housekeeping Thread) 会周期性地检查那些被借出但长时间未归还的连接。如果超过一个设定的阈值（例如 60 秒），连接池就会在日志中打印一条严重警告，包含当时借出连接的堆栈跟踪信息，帮助开发者迅速定位泄漏源。
//
// #### 5. 可配置性与可观测性 (Configuration & Observability)
//
// *   **缺少了什么？**：
//     *   从外部配置文件（如.ini, .xml, .json）加载所有参数（最大连接数、超时时间等）的能力。
//     *   暴露监控指标 (Metrics) 的能力，例如：当前活动连接数、空闲连接数、等待连接的线程数、平均等待时间等。这对于系统监控和性能调优至关重要。
//
// ### 总结对比
//
// | 特性 | 您的实现 (经典静态池) | 生产级连接池 (如 HikariCP) |
// | :--- | :--- | :--- |
// | **核心功能** | ✅ 线程安全的获取/归还 | ✅ |
// | **资源管理** | ✅ RAII 自动回收 | ✅ |
// | **连接健康检查** | ❌ 无 | ✅ (借出/归还/空闲时验证) |
// | **连接生命周期** | ❌ 连接永久存活 | ✅ (最大生命周期, 最大空闲时间) |
// | **获取超时** | ❌ 无限等待 | ✅ (可配置的超时时间) |
// | **泄漏检测** | ❌ 无 | ✅ (通过管家线程和堆栈跟踪) |
// | **动态调整** | ❌ 固定大小 | ✅ (通常支持最小/最大连接数动态伸缩) |
// | **监控与配置** | ❌ 硬编码 | ✅ (通过外部配置和暴露监控指标) |


