//
// Created by user on 2025/11/16.
//

#ifndef HTTP_DEFINE_H
#define HTTP_DEFINE_H
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <sys/mman.h>

namespace myhttp {

    /// 路径参数，如/user/123?include_details=true中的123是路径参数
    using PathParams = std::unordered_map<std::string, std::string>;
    /// 查询参数，如 /user/123?include_details=true中的include_detrails=true是查询参数
    using QueryParams = std::unordered_map<std::string, std::string>;


    /// 定义该HTTPConnection所支持的请求方法，暂时只支持GET和POST两种
    enum class Method {
        GET,
        POST,
        UNKNOWN
    };

    /// 定义工具函数将Method枚举类转化为字符串
    inline std::string serialize_method_kind(const Method &m) {
        switch (m) {
            case Method::GET: return "GET"; break;
            case Method::POST: return "POST"; break;
            case Method::UNKNOWN:
                default:
                    return "UNKNOWN";
        }
    }

    /// 定义Http请求结构体
    struct HttpRequest {
        Method method = Method::UNKNOWN;
        std::string uri; // 请求的资源路径
        std::string version; // HTTP协议版本
        std::unordered_map<std::string, std::string> headers; // 请求头集合（通过请求头名称唯一标识
        std::string body; // 请求体

        // 路径参数
        PathParams path_params;

        // 查询参数
        QueryParams query_params;

    };


    /// 定义HTTP响应状态码
    enum class HttpCode {
        OK = 200,
        BAD_REQUEST = 400,
        FORBIDDEN = 403,
        NOT_FOUND = 404,
        METHOD_NOT_ALLOWED = 405,
        INTERNAL_ERROR = 500
    };

    static const std::map<HttpCode, std::string_view> HttpCodeExplanations = {
        {HttpCode::OK, "OK"},
        {HttpCode::BAD_REQUEST, "Bad Request"},
        {HttpCode::FORBIDDEN, "Forbidden"},
        {HttpCode::NOT_FOUND, "Not Found"},
        {HttpCode::METHOD_NOT_ALLOWED, "Method Not Allowed"},
        {HttpCode::INTERNAL_ERROR, "Internal Error"}
    };

    /// 定义HTTP响应结构体
    struct HttpResponse {
        HttpCode status_code = HttpCode::OK; // 响应状态码
        std::unordered_map<std::string, std::string> headers; // 响应报头
        std::string body; // 响应体
        // 特殊成员，用于mmap文件
        struct MmappedFile {
            void* addr = nullptr;
            size_t size = 0;
            ~MmappedFile() {
                if(addr) munmap(addr, size);
            }
        } mmapped_file;

    public:
        /// 根据状态码构造一个HttpResponse
        /// 返回指向自己的引用，可以支持链式调用
        HttpResponse& status(const HttpCode code) {
            this->status_code = code;
            return *this;
        }

        /// 为响应头赋值
        HttpResponse& header(const std::string& key, const std::string& value) {
            this->headers[key] = value;
            return *this;
        }
        // 为了方便使用字符串字面量，可以增加一个 const char* 的重载
        HttpResponse& header(const char* key, const char* value) {
            this->headers[key] = value;
            return *this;
        }

        /// @brief 向响应体追加内容
        /// @param content 要追加的内容
        /// @return 返回自身的引用，以支持链式调用
        /// 向响应体中追加结果
        HttpResponse& append(const std::string_view content) {
            this->body.append(content);
            this->headers["Content-Length"] = std::to_string(this->body.size());
            return *this;
        }

        /// @brief 设置完整的响应体 (这是最常用的操作)
        /// @param content 完整的响应体内容
        /// @return 返回自身的引用，以支持链式调用
        /// 设置完整的响应体，这是最常用的操作
        HttpResponse& write(const std::string_view content) {
            this->body = content;
            this->headers["Content-Length"] = std::to_string(this->body.size());
            return *this;
        }

        /// @brief 一个便捷的函数，用于快速返回 JSON 响应
        /// @param json_content JSON 字符串
        /// @return 返回自身的引用，以支持链式调用
        HttpResponse& json(const std::string_view json_content) {
            this->header("Content-Type", "application/json");
            this->body = json_content;
            this->headers["Content-Length"] = std::to_string(this->body.size());
            return *this;
        }
    };

    // 定义一个ApiHandler，它是业务逻辑都需要遵循的接口
    using ApiHandler = std::function<void(const HttpRequest&, HttpResponse&)>;
}



#endif //HTTP_DEFINE_H
