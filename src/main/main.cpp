
#include <iostream>

// #include "controller.h"
#include "log/logger.h"
#include "net/EventLoop.h"
#include "net/EventLoopThreadPool.h"
// #include "webserver/webserver.h"

// // 辅助函数，用于打印使用说明
// void show_usage(const std::string& prog_name) {
//     std::cerr << "Usage: " << prog_name << " [options]\n"
//               << "Options:\n"
//               << "  -h, --help        Show this help message\n"
//               << "  -p <port>         Specify the server port (default: 9006)\n"
//               << "  -t <threads>      Specify the number of threads in the thread pool (default: 8)\n"
//               << "  -d <doc_root>     Specify the document root directory (e.g., ./resources)\n"
//               << std::endl;
// }
//
// std::optional<LogLevel> string_to_loglevel(const std::string& s) {
//     if (s == "DEBUG") return LogLevel::DEBUG;
//     if (s == "INFO") return LogLevel::INFO;
//     if (s == "WARN") return LogLevel::WARN;
//     if (s == "ERROR") return LogLevel::ERROR;
//     if (s == "NONE") return LogLevel::NONE;
//     return std::nullopt;
// }

// int main(int argc, char* argv[]) {
//     // ===================================================================
//     // 1. 初始化日志系统 (这是第一步，至关重要)
//     // ===================================================================
//     // 即使服务器启动失败，我们也希望能够记录下错误信息。
//     Logger::Config log_config;
//     log_config.log_folder = "/root/code/cpp/MyTinyWebServer/out/log"; // 日志文件存储的路径
//     log_config.max_queue_size = 1024;     // 开启异步日志
//     LogLevel default_level = LogLevel::INFO; // 设置默认日志等级
//     // note 在debug的时候默认设置为true，方便debuug
//     log_config.is_override = true;
//     log_config.enable_console_sink = true;
//     log_config.flush_interval_seconds = 0; // 同步刷新
//     Logger::get_instance().init(log_config);
//     // 1. 主线程创建自己的 Loop
//     EventLoop baseLoop;
//
//     // 2. 创建线程池，包含 2 个子线程
//     EventLoopThreadPool pool(&baseLoop, 2, "MyPool");
//     pool.start(); // 启动子线程
//
//     // 3. 获取下一个 Loop (轮询拿到 Loop-0)
//     EventLoop* subLoop = pool.getNextLoop();
//
//     // 4. 主线程命令 Loop-0 打印一句话
//     subLoop->runInLoop([](){
//         printf("Hello from SubThread!\n");
//     });
//
//     // 5. 主线程自己开始循环
//     baseLoop.loop();



    //
    // // 设置用户配置的日志等级
    // std::optional<LogLevel> user_log_level;
    //
    // // ===================================================================
    // // 2. 设置配置 (默认值 + 命令行解析)
    // // ===================================================================
    // ServerConfig server_config;
    //
    // // 从命令行解析参数来覆盖默认配置
    // // 这是一个简化的解析器，生产级应用可能会使用更强大的库（如 cxxopts）
    // for (int i = 1; i < argc; ++i) {
    //     std::string arg = argv[i];
    //     if (arg == "-h" || arg == "--help") {
    //         show_usage(argv[0]);
    //         return EXIT_SUCCESS;
    //     }
    //     if (arg == "-p" && i + 1 < argc) {
    //         try {
    //             server_config.port = std::stoi(argv[++i]);
    //         } catch (const std::exception& e) {
    //             LOG_ERROR("Invalid port number: %s", argv[i]);
    //             return EXIT_FAILURE;
    //         }
    //     } else if (arg == "-t" && i + 1 < argc) {
    //         try {
    //             server_config.thread_num = std::stoi(argv[++i]);
    //         } catch (const std::exception& e) {
    //             LOG_ERROR("Invalid thread number: %s", argv[i]);
    //             return EXIT_FAILURE;
    //         }
    //     } else if (arg == "-d" && i + 1 < argc) {
    //         server_config.m_root = argv[++i];
    //     } else if (arg == "-LOG" && i + 1 < argc) {
    //         std::string level_str = argv[++i];
    //         user_log_level = string_to_loglevel(level_str);
    //         if(!user_log_level) {
    //             LOG_ERROR("无效的日志等级，{}", level_str);
    //             return 0;
    //         }
    //     }
    // }
    //
    // // 默认配置root目录
    // if(server_config.m_root.empty()) {
    //     server_config.m_root = "/root/code/cpp/MyTinyWebServer/root";
    // }
    //
    // // 设置数据库
    // server_config.db_url = "127.0.0.1";
    // server_config.db_port = 3306;
    // server_config.db_user = "root";
    // server_config.db_password = "wang";
    // server_config.db_name = "yourdb";
    //
    // // todo 为了debug，将连接超时时间设置较长，例如10分钟，也就是600秒
    // server_config.connection_timeout = std::chrono::seconds(600);
    //
    // // 确保根目录有效
    // if (server_config.m_root.empty()) {
    //     LOG_ERROR("Document root directory (-d) must be specified.");
    //     show_usage(argv[0]);
    //     return EXIT_FAILURE;
    // }
    //
    // // ===================================================================
    // // 3. 创建并运行 WebServer (核心逻辑)
    // // ===================================================================
    // try {
    //     // 使用配置对象构造WebServer实例
    //     // 构造函数会完成所有资源的初始化（socket, epoll, 线程池等）
    //     // 如果任何一步失败，构造函数会抛出异常
    //     LOG_INFO("Initializing server with port {} and {} threads...",
    //              server_config.port, server_config.thread_num);
    //     WebServer server(server_config);
    //
    //
    //     // 注册handler
    //     server.get("/user/login", user_controller::login);
    //
    //     LOG_INFO("Server initialized successfully. Starting event loop...");
    //     // 将应用用户配置推迟到启动时间循环
    //     if(user_log_level) {
    //         Logger::get_instance().set_level(*user_log_level);
    //     }
    //     // 启动服务器的主事件循环
    //     // 这个函数会阻塞，直到服务器被关闭
    //     server.run();
    //
    // } catch (const std::exception& e) {
    //     // 程序结束后输出日志前，要退回到默认日志状态，防止用户更改为NONE而导致不显示的问题
    //     Logger::get_instance().set_level(default_level);
    //     // 捕获在服务器初始化或运行期间可能发生的任何标准异常
    //     LOG_ERROR("Server startup failed: {}", e.what());
    //     return EXIT_FAILURE;
    // }
    //
    // LOG_INFO("Server has shut down gracefully.");
    // return EXIT_SUCCESS;
// }