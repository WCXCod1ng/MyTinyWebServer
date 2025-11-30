//
// Created by user on 2025/11/16.
//

#include "WebRouter.h"

#include <format>
#include <sstream>

void WebRouter::add_route(const std::string& path, Method method, HandlerFunc handler) {
    // 1. 分割路径
    const auto segments = split_path(path);

    // 2. 从根节点开始遍历radix tree，并在遍历的过程中构建
    Node* current_node = m_root.get();

    for(size_t i = 0; i < segments.size(); ++i) {
        // 提取当前这一段
        const std::string& segment = segments[i];

        Node* next_node = nullptr;

        if(segment.empty()) continue; // 段不能为空

        // 根据段的类型查找和创建子节点，依据是段的第一个字符是否为“:”或“*”
        char first_char = segment[0];
        if(first_char == ':') {
            // 是参数节点
            if(!current_node->param_child) {
                // 子节点为空，创建参数一个子节点，它是参数节点，并且为其赋值
                current_node->param_child = std::make_unique<Node>();
                current_node->param_child->segment = segment;
                current_node->param_child->param_name = segment.substr(1); // 为名称赋值时不考虑“:”
            } else if (current_node->param_child->segment != segment) {
                // 如果子节点不为空，且与新加入的不一致，说明同一层出现了多个动态参数路由（例如/users/:id和/users/:name），这是不被允许的
                throw std::logic_error(std::format("Route conflict: Cannot have multiple parameter names({} and {}) at the same level.", segment, current_node->param_child->segment));
            } // 否则说明遇到相同的动态参数节点，允许

            next_node = current_node->param_child.get();
        } else if(first_char == '*') {
            // 是通配符节点
            // 必须是路径的最后一段
            if(i != segments.size() - 1) {
                throw std::logic_error("Wildcard '*' must be at the end of the route path.");
            }
            if(!current_node->wildcard_child) {
                // 子节点为空，创建并赋值
                current_node->wildcard_child = std::make_unique<Node>();
                current_node->wildcard_child->segment = segment;
                current_node->wildcard_child->param_name = segment.substr(1); // 同样，也不考虑“*”
            }

            next_node = current_node->wildcard_child.get();
        } else {
            // 是静态路由节点
            auto& static_children = current_node->static_children;
            if(!static_children.contains(segment)) {
                // 如果路径不存在，则创建新的静态子节点，并赋值
                static_children[segment] = std::make_unique<Node>();
                static_children[segment]->segment = segment;
            }

            next_node = static_children[segment].get();
        }

        // 移动到下一个节点
        current_node = next_node;
    }

    // 在最终的节点上注册handler
    if(current_node->handlers.contains(method)) {
        // 已经为该路径和方法注册过一个处理器了
        throw std::logic_error("Route conflict: Handler for this path and method already exists.");
    }
    current_node->handlers[method] = std::move(handler);
}

RouteResult WebRouter::find_route(const std::string &path, Method method) const {
    // 1. 初始化
    Node* current_node = m_root.get();
    std::unordered_map<std::string, std::string> params;
    const auto segments = split_path(path);

    // 2. 遍历请求路径的每一段
    for (size_t i = 0; i < segments.size(); ++i) {
        const std::string& segment = segments[i];

        // 3. 按照“静态 > 参数 > 通配符”的优先级进行匹配

        // 优先级 1: 查找静态子节点
        auto& static_children = current_node->static_children;
        auto it = static_children.find(segment);
        if (it != static_children.end()) {
            current_node = it->second.get();
            continue; // 匹配成功，继续处理下一个 segment
        }

        // 优先级 2: 查找参数子节点
        if (current_node->param_child) {
            current_node = current_node->param_child.get();
            // 捕获参数值，因为动态参数需要知道请求实际传递的是什么值。
            // 这一段被匹配上了，就说明这一段就会作为捕获参数的值
            params[current_node->param_name] = segment;
            continue; // 匹配成功，继续处理下一个 segment
        }

        // 优先级 3: 查找通配符子节点
        if (current_node->wildcard_child) {
            current_node = current_node->wildcard_child.get();
            // 捕获剩余的所有路径段
            std::stringstream remaining_path;
            for (size_t j = i; j < segments.size(); ++j) {
                remaining_path << segments[j];
                if (j < segments.size() - 1) {
                    remaining_path << "/"; // 通过斜杠分割，最后一个不加斜杠
                }
            }
            // 同样，也需要捕获参数的值，只不过被匹配上的是后续所有段（通配符的特点）
            params[current_node->param_name] = remaining_path.str();

            // 通配符匹配后，路径已经处理完毕，直接跳出循环
            goto found_node_path;
        }

        // 如果以上都未匹配成功，说明没有对应的路由
        return {RouteStatus::NOT_FOUND_URL, static_cast<HandlerFunc>(nullptr), {}};
    }

    // 到此说明匹配完毕
found_node_path:
    // 4. 如果该路径下没有注册任何方法，视为不是一个正确的URL，返回NOT_FOUND_URL
    if(current_node->handlers.empty()) {
        return {RouteStatus::NOT_FOUND_URL, nullptr, {}};
    }
    // 5. 路径完全匹配后，在最终节点上根据 HTTP 方法查找处理器
    auto handler_it = current_node->handlers.find(method);
    if (handler_it == current_node->handlers.end()) {
        // 路径匹配，但方法找不到 (405 Method Not Allowed)
        return {RouteStatus::NOT_FOUND_METHOD, nullptr, {}};
    }

    // 6. 找到了对应的处理器
    return {RouteStatus::FOUND, handler_it->second, std::move(params)};
}

std::vector<std::string> WebRouter::split_path(const std::string &path) {
    std::vector<std::string> segments;
    // 处理边界条件，即只由一个斜杠，此时segments也是有一个元素“/”
    if(path.empty() || path == "/") {
        return segments;
    }

    // 使用 stringstream 来高效地分割字符串
    std::stringstream ss(path);
    std::string segment;

    // C++11 特性: std::getline 可以方便地处理流式数据
    while (std::getline(ss, segment, '/')) {
        if (!segment.empty()) { // 可以处理第一个和最后一个斜杠的问题（相当于不处理）
            segments.push_back(segment);
        }
    }

    return segments;
}
