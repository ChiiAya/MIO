#pragma once

#include "nlohmann/json.hpp"
#include <cstdint>
#include <string>
namespace mio{
namespace mcp {

namespace ErrorCode {
    constexpr int ParseError     = -32700; // JSON 解析失败
    constexpr int InvalidRequest = -32600; // 请求结构不合规
    constexpr int MethodNotFound = -32601; // 方法不存在
    constexpr int InvalidParams  = -32602; // 参数无效
    constexpr int InternalError  = -32603; // 服务端内部错误
}

struct JsonRpcError {
    int code = 0;
    std::string message;
    nlohmann::json data = nullptr;

    bool empty() const { return code == 0 && message.empty(); }
};

struct JsonRpcRequest{
    std::int64_t id = 0;
    std::string method;
    nlohmann::json params = nlohmann::json::object();

    nlohmann::json toJson() const {
        nlohmann::json j = {
            {"jsonrpc","2.0"},
            {"id", id},
            {"method", method}
        };
        if(!params.is_null() && !params.empty()){
            j["params"] = params;
        }else{
            j["params"] = nlohmann::json::object();
        }
        return j;
    }
};

struct JsonRpcNotification {
    std::string method;
    nlohmann::json params = nlohmann::json::object();

    nlohmann::json toJson() const {
        nlohmann::json j = {
            {"jsonrpc", "2.0"},
            {"method", method}
        };
        if (!params.is_null() && !params.empty()) {
            j["params"] = params;
        }
        return j;
    }
};


struct JsonRpcResponse {
    std::int64_t id = 0;
    nlohmann::json result = nullptr;
    std::optional<JsonRpcError> error = std::nullopt;

    bool isSuccess() const { return !error.has_value(); }

    static JsonRpcResponse fromJson(const nlohmann::json& j) {
        JsonRpcResponse resp;
        if (j.contains("id") && j["id"].is_number()) {
            resp.id = j["id"].get<std::int64_t>();
        }
        if (j.contains("result")) {
            resp.result = j["result"];
        }
        if (j.contains("error") && j["error"].is_object()) {
            JsonRpcError err;
            const auto& ej = j["error"];
            err.code = ej.value("code", 0);
            err.message = ej.value("message", "");
            if (ej.contains("data")) {
                err.data = ej["data"];
            }
            resp.error = err;
        }
        return resp;
    }
};

}//namespace mcp
}//namespace mio 