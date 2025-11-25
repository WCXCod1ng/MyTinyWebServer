cmake_minimum_required(VERSION 3.22)
project(MyTinyWebServer)
set(CMAKE_CXX_STANDARD 20)


# =======================================================
# 集成 GoogleTest
# =======================================================

# 1. 包含 FetchContent 模块
include(FetchContent)


# 2. 声明要集成的外部项目

# =======================================================
# 集成 GoogleTest
# =======================================================
FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.13.0 # 使用一个具体的、稳定的版本号！
)

# =======================================================
# 集成 Cereal 库
# =======================================================
FetchContent_Declare(
        cereal
        GIT_REPOSITORY https://github.com/USCiLab/cereal.git
        GIT_TAG        v1.3.2
)


# =======================================================
# 集成 nlohmann/json 库
# =======================================================
FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG "v3.11.3" # 使用一个具体的、稳定的版本标签
)


# 3. 配置依赖并使其可用
#    这一步会触发下载和配置
FetchContent_MakeAvailable(
        googletest
        cereal
        nlohmann_json
)

## =======================================================
## 集成 MySQL C++ Connector (新添加的部分)
## =======================================================
#FetchContent_Declare(
#        mysql_connector_cpp
#        GIT_REPOSITORY https://github.com/mysql/mysql-connector-cpp.git
#        GIT_TAG        8.4.0 # 使用一个官方发布的稳定版本号
#)
#
## 关键 —— 强制使用内置 Protobuf，避免系统 Protobuf 冲突
#set(WITH_PROTOBUF bundled CACHE INTERNAL "")

# =======================================================

enable_testing()

# ===========================
# MySQL client (libmysqlclient)
# ===========================
#find_package(MySQL REQUIRED)
## 或手动设置（如果 find_package 找不到）
## set(MYSQL_INCLUDE_DIR /usr/include/mysql)
## set(MYSQL_LIBRARIES /usr/lib/x86_64-linux-gnu/libmysqlclient.so)
#include_directories(${MYSQL_INCLUDE_DIR})
include_directories(/usr/include/mysql)
link_directories(/usr/lib/x86_64-linux-gnu)



# 添加源文件目录
add_subdirectory(src)
