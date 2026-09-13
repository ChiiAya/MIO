#include "admin/AdminServer.h"
#include "config/AppConfig.h"
#include "log/Log.h"
#include "runtime/Runtime.h"

#include <ixwebsocket/IXHttpServer.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace mio {

AdminServer::AdminServer(Runtime& runtime, int port, std::string host)
    : runtime_(runtime), port_(port), host_(std::move(host)) {}

AdminServer::~AdminServer() {
    stop();
}

bool AdminServer::start() {
    if (running_) return true;
    if (port_ <= 0) {
        log::info("AdminServer", "adminPort <= 0，管理服务已禁用");
        return false;
    }

    try {
        server_ = std::make_unique<ix::HttpServer>(port_, host_);
        server_->setOnConnectionCallback([this](ix::HttpRequestPtr req,
                                                std::shared_ptr<ix::ConnectionState>)
                                             -> ix::HttpResponsePtr {
            ix::WebSocketHttpHeaders headers;
            headers["Content-Type"] = "application/json; charset=utf-8";
            headers["Access-Control-Allow-Origin"] = "*";
            headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS";
            headers["Access-Control-Allow-Headers"] = "Content-Type";

            if (req->method == "OPTIONS") {
                return std::make_shared<ix::HttpResponse>(200, "OK", ix::HttpErrorCode::Ok, headers, "");
            }

            // GET /api/status: 机器人状态 + 当前生效提示词
            if (req->uri == "/api/status" && req->method == "GET") {
                nlohmann::json j;
                j["ok"] = true;
                auto cfg = runtime_.config();
                j["botName"] = cfg ? cfg->botName : "Mio";
                j["platform"] = cfg ? cfg->platform : "console";
                j["adminPort"] = port_;
                j["systemPrompt"] = runtime_.systemPrompt();
                j["messageCount"] = runtime_.state().messageCount;

                return std::make_shared<ix::HttpResponse>(200, "OK", ix::HttpErrorCode::Ok,
                                                          headers, j.dump());
            }

            // GET /api/config: 返回当前生效的 AppConfig
            if (req->uri == "/api/config" && req->method == "GET") {
                auto cfg = runtime_.config();
                nlohmann::json j;
                if (cfg) {
                    to_json(j, *cfg);
                } else {
                    j = nlohmann::json::object();
                }
                return std::make_shared<ix::HttpResponse>(200, "OK", ix::HttpErrorCode::Ok, headers, j.dump(2));
            }

            // POST /api/config: 写入新配置并触发热重载
            // POST /api/reload: 触发热重载
            if ((req->uri == "/api/config" || req->uri == "/api/reload") && req->method == "POST") {
                if (req->uri == "/api/config" && !req->body.empty()) {
                    try {
                        auto parsed = nlohmann::json::parse(req->body, nullptr, true, true);
                        std::ofstream f("config.json");
                        if (!f.is_open()) {
                            nlohmann::json err = {{"ok", false}, {"error", "无法写入 config.json"}};
                            return std::make_shared<ix::HttpResponse>(500, "Error", ix::HttpErrorCode::Ok, headers, err.dump());
                        }
                        f << parsed.dump(2) << "\n";
                        f.close();
                    } catch (const std::exception& e) {
                        nlohmann::json err = {{"ok", false}, {"error", std::string("JSON 解析错误: ") + e.what()}};
                        return std::make_shared<ix::HttpResponse>(400, "Bad Request", ix::HttpErrorCode::Ok, headers, err.dump());
                    }
                }

                bool ok = runtime_.reloadConfig("config.json");
                nlohmann::json resp = {
                    {"ok", ok},
                    {"message", ok ? "配置热重载成功并即时生效" : "配置重载失败，已保持原配置"}
                };
                return std::make_shared<ix::HttpResponse>(ok ? 200 : 500, ok ? "OK" : "Error", ix::HttpErrorCode::Ok, headers, resp.dump());
            }

            nlohmann::json notFound = {{"ok", false}, {"error", "Not Found"}};
            return std::make_shared<ix::HttpResponse>(404, "Not Found", ix::HttpErrorCode::Ok, headers, notFound.dump());
        });

        auto res = server_->listen();
        if (!res.first) {
            log::warn("AdminServer", "监听端口失败: " + host_ + ":" + std::to_string(port_) + " - " + res.second);
            server_.reset();
            return false;
        }
        server_->start();
        running_ = true;
        log::info("AdminServer", "管理服务已启动: http://" + host_ + ":" + std::to_string(port_));
        return true;
    } catch (const std::exception& e) {
        log::warn("AdminServer", std::string("启动异常: ") + e.what());
        server_.reset();
        return false;
    }
}

void AdminServer::stop() {
    if (running_ && server_) {
        log::info("AdminServer", "停止管理服务");
        server_->stop();
        running_ = false;
        server_.reset();
    }
}

} // namespace mio

