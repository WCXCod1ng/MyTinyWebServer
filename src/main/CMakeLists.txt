# 1. 创建一个可执行文件目标
#    目标名称为 "TinyWebServerApp"，源文件为 main.cpp
add_executable(TinyWebServerApp main.cpp)

# 2. 链接该可执行文件所需要的所有库
#    这里我们链接在 lib 目录中创建的 WebServerLib 库
#    PRIVATE 表示 WebServerLib 是 TinyWebServerApp 的私有依赖
target_link_libraries(TinyWebServerApp PRIVATE WebServerLib)