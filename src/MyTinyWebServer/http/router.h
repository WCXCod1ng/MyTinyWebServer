//
// Created by user on 2025/11/16.
//

#ifndef ROUTER_H
#define ROUTER_H
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "http_define.h"

enum class RouteStatus {
    FOUND = 0, // 找到
    NOT_FOUND_URL = 1, // 路径找不到
    NOT_FOUND_METHOD = 2, // 路径可以匹配上，但方法找不到
};

/// 定义路由查找结果的类型
/// 这里还使用一个map的原因是为了针对动态参数和通配符匹配，当使用这两种方式时，需要知道它们分别匹配了什么内容（业务handler才能进一步处理）
/// 为了支持不同请求方式，还需要一个返回值表明请求状态：是否找到，以及没有找到的原因（路径不匹配、路径匹配但是没有对应的方法）
/// 只有当0位置是FOUND，其他下标的值才不为空
using RouteResult = std::tuple<RouteStatus, myhttp::ApiHandler, std::unordered_map<std::string, std::string>>;

/// 当前使用字典树实现路由
/// todo 使用基数树实现路由（请求分发），采用了基数树后，支持更多的路由功能（按照优先级降序，即先按照静态路由匹配，然后是动态参数，最后是通配符）：
/// 1. 静态路由 (/users/profile)：这是最基本的功能
/// 2. 动态参数 (/users/:id)
/// 3. 通配符 (/static/*filepath)
/// 基数树是字典树Trie的一种优化版本，优化点在于：它会对路径上的点进行压缩，如果系统中只有 /api/v1/users 和 /api/v1/orders 两条路由，基数树的结果如下：
/// /
/// └── "api/v1/" (合并了的边)
///      ├── "users" -> [存储 /api/v1/users 的处理器]
///      └── "orders" -> [存储 /api/v1/orders 的处理器]
class Router {
public:
    // 构造函数
    Router() {
        // 创建一个根节点，代表根目录“/”，作为所有路径的起点
        m_root = std::make_unique<Node>();
    }

    /// 添加一条路由规则
    /// 核心的路由注册函数。它会接收一个路径、HTTP方法和处理器，然后解析路径，遍历树，在适当的位置创建新节点，并最终将处理器存放在目标节点的 handlers 映射中
    void add_route(const std::string& path, myhttp::Method method, myhttp::ApiHandler handler);

    // 方便的辅助函数
    void get(const std::string& path, myhttp::ApiHandler handler) {
        // C++11 特性: std::move 用于将 handler 的所有权“移动”给 add_route，
        // 避免了不必要的拷贝，提高了效率。
        add_route(path, myhttp::Method::GET, std::move(handler));
    }
    void post(const std::string& path, myhttp::ApiHandler handler) {
        add_route(path, myhttp::Method::POST, std::move(handler));
    }
    // ... 可以为其他 HTTP 方法添加类似函数 ...

    /// 查找匹配的路由
    /// 核心的路由查找函数。它接收一个请求路径和方法，从根节点开始遍历树。在遍历过程中，它会遵循“静态 > 动态 > 通配符”的优先级规则进行匹配。
    /// 如果匹配成功，它会收集路径中的动态参数，并返回找到的处理器和参数集。如果找不到，返回一个空的处理器
    /// @param path 请求的路径，例如 "/users/123"
    /// @param method 请求的方法，例如 Method::GET
    /// @return 返回一个 pair:
    ///         - .first: 找到的 ApiHandler (如果没找到则为空)
    ///         - .second: 从路径中解析出的参数 map (例如 {"id": "123"})
    [[nodiscard]] RouteResult find_route(const std::string& path, myhttp::Method method) const;

private:
    // 内部的树节点结构
    struct Node {
        // 该节点代表的路径段 (被压缩的部分)，例如api/v1/
        std::string segment;

        // 存储该节点对应的不同 HTTP 方法的处理器，基数树只能根据路径判断，如果需要根据方法的不同选择不同的处理逻辑，就需要在每个节点内部维护一个不同方法到handler的映射
        std::unordered_map<myhttp::Method, myhttp::ApiHandler> handlers;

        // 子节点
        // 使用 C++11 智能指针 std::unique_ptr 自动管理内存
        // 存储所有静态路径的子节点，key 是子路径段。例如，一个代表 /users 的节点，它的 static_children 中可能有一个 key 为 "profile" 的子节点，指向 /users/profile
        std::unordered_map<std::string, std::unique_ptr<Node>> static_children;
        // 存储动态参数的子节点 (例如 :id)。一个节点最多只能有一个这样的子节点。例如，在 /users/:id 路由中，代表 users 的节点会有一个 param_child 指向代表 :id 的节点
        std::unique_ptr<Node> param_child = nullptr;
        // 存储通配符的子节点 (例如 *filepath)
        std::unique_ptr<Node> wildcard_child = nullptr;

        // 如果是参数节点，需要存储参数名
        // 当这个节点是动态参数节点（例如 :id）或通配符节点（*filepath）时，这个字段存储参数的名称（"id" 或 "filepath"），以便在匹配时能够将捕获到的值存入结果 map 中
        std::string param_name;
    };

    // 路由树的根节点
    std::unique_ptr<Node> m_root;

    /// 内部辅助工具函数，用于将像 "/users/:id/posts" 这样的路径字符串分割成 ["users", ":id", "posts"] 的段列表，方便后续在树中进行遍历
    static std::vector<std::string> split_path(const std::string& path);
};



#endif //ROUTER_H
