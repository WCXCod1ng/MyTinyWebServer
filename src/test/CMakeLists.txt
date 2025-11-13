
# 1. 创建用于测试的可执行文件
add_executable(run_tests
        cpp/lock_test.cpp
        cpp/mysql_connection_pool_test.cpp
        ../../src/lib/mysql_connection_pool/mysql_connection_pool.cpp
        cpp/thread_pool_test.cpp
        cpp/block_queue_test.cpp
        ../../src/lib/log/logger.cpp
        cpp/logger_test.cpp
)

# 2. 链接测试程序需要的库
#    首先链接我们自己的 WebServerLib
#target_link_libraries(run_tests PRIVATE WebServerLib)

# 3. 链接 GoogleTest 库
#    GTest::gtest_main 包含了 GTest 库本身以及一个预编译好的 main 函数
target_link_libraries(run_tests
        PRIVATE
        GTest::gtest_main
        mysqlclient
)

# 4. 将可执行文件注册为 CTest 测试用例 (这一步保持不变)
add_test(NAME ServerLogicTest COMMAND run_tests)