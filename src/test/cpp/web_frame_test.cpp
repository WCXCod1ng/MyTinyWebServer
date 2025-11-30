//
// Created by user on 2025/11/30.
//


#include <http/WebFrame.h>
#include <log/logger.h>
#include <net/InetAddress.h>

int main() {
    const InetAddress addr(9006);
    WebFrame app(addr, "SmartWeb");

    // 注册GET方法
    app.GET("/user/:id", [](Context& ctx) {
        if(const auto user_id = ctx.pathVariable("id")) {
            ctx.JSON(HttpStatusCode::k200Ok, "{\"id\": " + *user_id + "}");
        } else {
            throw std::runtime_error("异常，没有匹配到任何内容");
        }
    });

    // 测试异常处理
    app.POST("/panic", [](const Context& ctx) {
       throw std::runtime_error("故意抛出一个异常");
    });

    // 测试查询参数
    app.GET("/user/query", [](const Context& ctx) {
        if (const auto name = ctx.query("name")) {
            ctx.STR(HttpStatusCode::k200Ok, "hello " + *name);
        }
    });

    // 自定义全局异常处理 (覆盖默认行为)
    app.setExceptionHandler([](Context& ctx, const std::exception& e) {
        // 比如记录到日志文件
        LOG_ERROR("Global Exception: {}", e.what());
        // 返回友好的 JSON 错误信息
        ctx.JSON(HttpStatusCode::k500InternalServerError, "{\"error\": \"系统繁忙，请稍后再试\"}");
    });

    // 自定义 404 页面
    app.setNotFoundHandler([](Context& ctx) {
        ctx.resp()->setStatusCode(HttpStatusCode::k404NotFound);
        ctx.resp()->setBody("<h1>My Custom 404 Page</h1>");
    });

    // 自定义 405 页面
    app.setMethodNotAllowedHandler([](Context& ctx) {
        ctx.resp()->setStatusCode(HttpStatusCode::k405MethodNotAllowed);
        ctx.resp()->setBody("<h1>My Custom 405 Page</h1>");
    });


    app.start();

    return 0;
}
