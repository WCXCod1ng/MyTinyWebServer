//
// Created by user on 2025/11/17.
//

#ifndef ERROR_HANDLER_H
#define ERROR_HANDLER_H
#include <functional>
#include <string>
#include <cereal/archives/json.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/string.hpp>

#include "restful.h"

namespace GlobalExceptionHandler {

    // 定义处理函数的类型签名：输入一个标准异常引用，输出一个序列化后的字符串
    using HandlerFunc = std::function<std::string(const std::exception&)>;

    /**
     * @brief 默认的全局异常处理器.
     *
     * 这个函数捕获到一个异常后，会将其格式化为一个标准的、表示错误的 Result JSON.
     * @param e 捕获到的异常对象.
     * @return std::string 序列化后的JSON字符串.
     */
    inline std::string defaultHandler(const std::exception& e) {
        std::stringstream ss;
        // 1. 获取当前系统时间，并转换为 UTC 时间点
        //    C++20 的 clock_cast 可以安全地在不同时钟/时区之间转换
        const auto now_utc = std::chrono::clock_cast<std::chrono::utc_clock>(std::chrono::system_clock::now());
        // 2. 将时间点截断到毫秒精度
        const auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now_utc);
        auto timestamp = std::format("{:%Y-%m-%dT%H:%M:%S}", now_ms);
        {
            std::unordered_map<std::string, std::string> error_json= {
                {"timestamp", timestamp},
                {"status", "500"},
                {"error", std::format("Internal Server Error: {}", e.what())}
            };
            // 创建一个JSON输出归档
            cereal::JSONOutputArchive archive(ss, cereal::JSONOutputArchive::Options::Default());

            // 序列化map
            archive(error_json);
        }
        return ss.str();
    }

    // 全局的、可修改的处理函数。C++17的inline变量特性确保它在整个程序中只有一个实例。
    // 它被初始化为我们的默认实现。
    inline HandlerFunc currentHandler = defaultHandler;

    /**
     * @brief 允许用户设置自定义的异常处理函数.
     * 这是实现“覆盖”功能的核心。
     * @param newHandler 用户提供的新的处理函数.
     */
    inline void setHandler(HandlerFunc newHandler) {
        if (newHandler) {
            currentHandler = std::move(newHandler);
        } else {
            // 如果用户提供空函数，则恢复默认
            currentHandler = defaultHandler;
        }
    }

    /**
     * @brief 在try-catch块中调用的处理入口.
     *
     * 它会调用当前被设置的处理函数 (currentHandler).
     * @param e 捕获到的异常.
     * @return std::string 序列化后的JSON响应.
     */
    inline std::string process(const std::exception& e) {
        return currentHandler(e);
    }

} // namespace GlobalExceptionHandler

#endif //ERROR_HANDLER_H
