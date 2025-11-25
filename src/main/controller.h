//
// Created by user on 2025/11/17.
//

// 定义controller，需要哦满足http_define.h的规范

#include <functional>
#include <string>
#include <optional>
#include <nlohmann/json.hpp>
#include "http/http_define.h"
#include "utils/restful.h"

namespace user_controller {
    void login(const myhttp::HttpRequest& request, myhttp::HttpResponse& response) {
        const std::string& username = request.query_params.at("username");
        const std::string& password = request.query_params.at("password");


        if(username == "admin" && password == "123456") {
            const utils::Result<std::string> res = utils::make_success_result(200, std::string(""), std::string("token"));
            nlohmann::json j_res = res;
            response.status(myhttp::HttpCode::OK).json(j_res.dump(4));
        }
    }
}
