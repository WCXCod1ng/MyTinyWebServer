//
// Created by user on 2025/11/17.
//

#ifndef RESTFUL_H
#define RESTFUL_H
#include <optional>
#include <string>
#include "nlohmann/json.hpp"

namespace utils {
    /**
 * @brief 通用的API响应结构体，类似于Spring的Result类.
 * @tparam T 成功时，data字段的类型.
 */
    template <typename T>
    struct Result {
        bool success;
        int code;
        std::string message;
        std::optional<T> data; // 使用 std::optional 来表示数据可能不存在
    };


    // 序列化函数：将 Result<T> 对象转换为 json 对象
    // 必须是模板化的自由函数
    template <typename T>
    void to_json(nlohmann::json& j, const Result<T>& r) {
        j = nlohmann::json{
                {"success", r.success},
                {"code", r.code},
                {"message", r.message}
        };

        // nlohmann/json 原生支持 std::optional
        // 如果 r.data 包含值，它会被序列化
        // 如果 r.data 是 std::nullopt，它会被序列化为 JSON null
        // j["data"] = r.data; // 在3.11.3中不支持这样的写法
        if(r.data) {
            j["data"] = *r.data;
        } else {
            j["data"] = nullptr;
        }
    }

    // 反序列化函数：将 json 对象转换为 Result<T> 对象
    // 同样必须是模板化的自由函数
    template <typename T>
    void from_json(const nlohmann::json& j, Result<T>& r) {
        j.at("success").get_to(r.success);
        j.at("code").get_to(r.code);
        j.at("message").get_to(r.message);

        // 同样，原生支持从 json 值或 json null 反序列化到 std::optional
        // 如果json中 "data" 键的值是 null，r.data 将会是 std::nullopt
        // 否则，它会尝试将 "data" 的值反序列化为类型 T
        // j.at("data").get_to(r.data); // 在3.11.3中不支持这样的写法
        if(j.contains("data") && !j["data"].is_null()) {
            r.data = j["data"].get<T>();
        } else {
            r.data = std::nullopt;
        }
    }


    /**
     * @brief 用于创建成功的Result对象的辅助函数.
     */
    template <typename T>
    Result<T> make_success_result(int code, const std::string& message, T&& data) {
        return {true, code, message, std::forward<T>(data)};
    }


    /**
     * @brief 用于创建失败的Result对象的辅助函数.
     * 注意：失败时，数据类型为 std::nullptr_t，表示没有有效数据.
     */
    inline Result<std::nullptr_t> make_error_result(const int code, const std::string& message) {
        return {false, code, message, std::nullopt};
    }
}



#endif //RESTFUL_H
