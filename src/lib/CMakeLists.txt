# 1. 定义组成这个库的所有源文件
#    当未来有更多模块时，只需在这里添加它们的 .cpp 文件路径
set(LIB_SOURCES
        mysql_connection_pool/mysql_connection_pool.cpp
        mysql_connection_pool/mysql_connection_pool.h
        thread_pool/thread_pool.h
        webserver/webserver.cpp
        webserver/webserver.h
        http/http_conn.cpp
        http/http_conn.h
        log/logger.cpp
        log/logger.h
        utils/timer.cpp
        utils/timer.h
        utils/block_queue.h
        utils/concurrent_queue.h
        # logger/logger.cpp  <-- 以后可以像这样添加更多源文件
        # thread_pool/thread_pool.cpp
)

# 2. 创建一个库目标
#    名称为 WebServerLib，源文件列表来自上面的变量
add_library(WebServerLib ${LIB_SOURCES})


# 4. 指定头文件的搜索路径
#    这至关重要，它告诉链接到 WebServerLib 的任何目标（main, test）
#    应该到哪里去寻找 #include "..." 的头文件
#    PUBLIC 关键字意味着这个设置会“传递”给链接者
target_include_directories(WebServerLib
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# 5. 链接外部依赖库
#    将我们的 WebServerLib 链接到 MySQL 客户端库
#    PUBLIC 关键字确保任何链接到 WebServerLib 的目标也能正确链接到 MySQL
target_link_libraries(WebServerLib
        PUBLIC
        mysqlclient
)