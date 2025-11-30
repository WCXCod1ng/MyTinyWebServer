# 1. 定义组成这个库的所有源文件，注意只能添加 .cpp 文件
# 可以使用file会自动查找所有 .cpp 文件
#    这将递归地在当前目录 (src/MyWebServer) 下查找所有 .cpp 文件
#file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS "*.cpp")
# 但是为了清晰起见，这里我们手动列出所有源文件
set(LIB_SOURCES
        log/logger.cpp
        log/file_sink.cpp
        base/Buffer.cpp
        net/Channel.cpp
        net/EventLoop.cpp
        net/EpollPoller.cpp
        net/EventLoopThread.cpp
        net/EventLoopThreadPool.cpp
        net/InetAddress.cpp
        net/Socket.cpp
        net/Acceptor.cpp
        net/TcpConnection.cpp
        net/TcpServer.cpp
        net/Timer.cpp
        net/TimerQueue.cpp
        http/HttpContext.cpp
        http/HttpServer.cpp
        http/WebRouter.cpp
        http/WebFrame.cpp
        mysql_connection_pool/mysql_connection_pool.cpp
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
        cereal::cereal
        nlohmann_json::nlohmann_json
)