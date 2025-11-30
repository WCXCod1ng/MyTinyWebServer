
# 为run_tests可执行文件添加CMake配置
add_executable(run_tests
        cpp/lock_test.cpp
        cpp/mysql_connection_pool_test.cpp
        cpp/thread_pool_test.cpp
        cpp/block_queue_test.cpp
        cpp/logger_test.cpp
        cpp/simple_server_test.cpp
        cpp/web_frame_test.cpp
)
target_link_libraries(run_tests PRIVATE WebServerLib)


# 为http_server_test可执行文件添加CMake配置
add_executable(http_server_test
        cpp/http_server_test.cpp
)
target_link_libraries(http_server_test PRIVATE WebServerLib)

# 3. 链接 GoogleTest 库
#    GTest::gtest_main 包含了 GTest 库本身以及一个预编译好的 main 函数
target_link_libraries(run_tests
        PRIVATE
        GTest::gtest_main
        mysqlclient
)

# 4. 将可执行文件注册为 CTest 测试用例 (这一步保持不变)
add_test(NAME ServerLogicTest COMMAND run_tests)