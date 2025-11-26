//
// Created by user on 2025/11/11.
//
#include "../../MyWebServer/mysql_connection_pool/mysql_connection_pool.h"
#include "gtest/gtest.h"
#include <mysql/mysql.h>
#include <thread>
#include <vector>
#include <chrono>
#include <sstream>

// --- 测试固件 (Test Fixture) ---
// 用于管理测试的通用设置和清理工作
class ConnectionPoolTest : public ::testing::Test {
protected:
    // 测试所需常量
    // 当您在 mysql_real_connect 函数中，将主机名参数指定为字符串 "localhost" 时，MySQL 客户端库不会尝试通过 TCP/IP 网络协议去连接 3306 端口。相反，它会认为数据库服务器就在本机上，并尝试通过一种更高效的本地进程间通信方式——Unix Domain Socket——去连接。
    // 错误信息中的这一段就是铁证： Can't connect to local MySQL server through socket '/var/run/mysqld/mysqld.sock' (2)
    const char* host = "127.0.0.1";
    const int port = 3306;
    const char* user = "root";
    const char* pass = "wang"; // 请替换为您的密码
    const char* db_name = "test";
    const int pool_size = 4; // 为测试设置一个较小的池大小

    // SetUp() 在每个 TEST_F 运行前被调用
    void SetUp() override {
        // 1. 建立一个独立的、临时的MySQL连接，用于创建测试环境
        MYSQL* setup_conn = mysql_init(nullptr);
        ASSERT_NE(setup_conn, nullptr) << "mysql_init failed.";

        // 连接到MySQL服务器（不指定数据库）
        ASSERT_NE(mysql_real_connect(setup_conn, host, user, pass, nullptr, port, nullptr, 0), nullptr)
            << "Setup failed: Could not connect to MySQL server. Error: " << mysql_error(setup_conn);

        // 2. 执行SQL语句，确保测试环境干净
        ASSERT_EQ(mysql_query(setup_conn, "DROP DATABASE IF EXISTS test"), 0)
            << "DROP DATABASE failed: " << mysql_error(setup_conn);
        ASSERT_EQ(mysql_query(setup_conn, "CREATE DATABASE test"), 0)
            << "CREATE DATABASE failed: " << mysql_error(setup_conn);
        ASSERT_EQ(mysql_select_db(setup_conn, "test"), 0)
            << "mysql_select_db failed: " << mysql_error(setup_conn);

        const char* create_table_sql = "CREATE TABLE user (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50))";
        ASSERT_EQ(mysql_query(setup_conn, create_table_sql), 0)
            << "CREATE TABLE failed: " << mysql_error(setup_conn);

        // 3. 关闭临时连接
        mysql_close(setup_conn);

        // 4. 初始化我们真正要测试的连接池
        mysql_conn_pool::get_instance().init(host, user, pass, db_name, port, pool_size);
    }

    // TearDown() 在每个 TEST_F 运行后被调用
    void TearDown() override {
        // 清理连接池（虽然单例析构函数会自动调用，但显式调用可以确保顺序）
        mysql_conn_pool::get_instance().destroy_pool();

        // 建立临时连接以删除测试数据库
        MYSQL* teardown_conn = mysql_init(nullptr);
        ASSERT_NE(teardown_conn, nullptr);
        ASSERT_NE(mysql_real_connect(teardown_conn, host, user, pass, nullptr, port, nullptr, 0), nullptr);
        ASSERT_EQ(mysql_query(teardown_conn, "DROP DATABASE IF EXISTS test"), 0);
        mysql_close(teardown_conn);
    }
};

// --- 测试用例 ---

// 测试1：基本连接获取与操作
// 验证可以成功获取一个连接，执行SQL语句，并且连接会自动归还。
TEST_F(ConnectionPoolTest, BasicConnectionAndQuery) {
    // 从池中获取一个连接
    auto conn = mysql_conn_pool::get_instance().get_connection();

    // 断言获取到的连接是有效的 (unique_ptr 的布尔转换)
    ASSERT_TRUE(conn) << "Failed to get a connection from the pool.";

    // 使用 conn->get() 获取裸指针来调用C API
    const char* insert_sql = "INSERT INTO user (name) VALUES ('gtest_user')";
    int ret = mysql_query(conn.get(), insert_sql);

    // 断言SQL执行成功
    ASSERT_EQ(ret, 0) << "INSERT query failed: " << mysql_error(conn.get());

    // conn 在离开作用域时会自动销毁，并将连接归还给池
}

// 测试2：顺序获取与释放
// 验证可以从池中取出所有可用连接，并在它们被释放后，可以再次全部取出。
TEST_F(ConnectionPoolTest, SequentialAcquireAndRelease) {
    std::vector<ScopedConnection> connections;
    connections.reserve(pool_size);

    // 1. 取出所有连接
    for (int i = 0; i < pool_size; ++i) {
        auto conn = mysql_conn_pool::get_instance().get_connection();
        ASSERT_TRUE(conn) << "Failed to get connection #" << i + 1;
        connections.push_back(std::move(conn));
    }
    ASSERT_EQ(connections.size(), pool_size);

    // 2. 清空vector，这将导致所有 ScopedConnection 对象被销毁，从而将连接归还给池
    connections.clear();

    // 3. 再次尝试取出所有连接，以验证它们都已被成功归还
    for (int i = 0; i < pool_size; ++i) {
        auto conn = mysql_conn_pool::get_instance().get_connection();
        ASSERT_TRUE(conn) << "Failed to get connection again #" << i + 1;
        connections.push_back(std::move(conn));
    }
    ASSERT_EQ(connections.size(), pool_size);
}


// 测试3：并发压力测试
// 模拟比池中连接数更多的线程同时请求连接，验证池的阻塞和并发处理能力。
TEST_F(ConnectionPoolTest, ConcurrentPressureTest) {
    // 启动比池大小两倍还多的线程，确保产生竞争和等待
    const int num_threads = pool_size * 2 + 2;
    std::vector<std::thread> threads;

    // 定义每个线程要执行的任务
    auto worker_task = []() {
        auto conn = mysql_conn_pool::get_instance().get_connection();
        ASSERT_TRUE(conn); // 每个线程都必须最终能获取到连接

        // 构造一个简单的插入语句
        std::stringstream ss;
        ss << "INSERT INTO user (name) VALUES ('thread_worker');";

        ASSERT_EQ(mysql_query(conn.get(), ss.str().c_str()), 0)
            << "Concurrent INSERT failed: " << mysql_error(conn.get());

        // 模拟一些业务处理，持有连接一段时间
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    };

    // 启动所有线程
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker_task);
    }

    // 等待所有线程执行完毕
    for (auto& th : threads) {
        th.join();
    }

    // 验证结果：所有线程都应该成功插入了一条记录
    auto conn = mysql_conn_pool::get_instance().get_connection();
    ASSERT_TRUE(conn);

    ASSERT_EQ(mysql_query(conn.get(), "SELECT COUNT(*) FROM user"), 0);

    MYSQL_RES* result = mysql_store_result(conn.get());
    ASSERT_NE(result, nullptr);

    MYSQL_ROW row = mysql_fetch_row(result);
    ASSERT_NE(row, nullptr);

    // 数据库中的记录数应等于线程数
    EXPECT_EQ(std::stoi(row[0]), num_threads);

    mysql_free_result(result);
}