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

            // GET /api/status: 机器人状态 + 活跃 FusionUnits 与 Token 消耗指标
            if (req->uri == "/api/status" && req->method == "GET") {
                nlohmann::json j;
                j["ok"] = true;
                auto cfg = runtime_.config();
                j["botName"] = cfg ? cfg->botName : "Mio";
                j["platform"] = cfg ? cfg->platform : "console";
                j["adminPort"] = port_;
                j["systemPrompt"] = runtime_.systemPrompt();

                j["units"] = nlohmann::json::array();
                auto units = runtime_.fusionUnitsInfo();
                for (const auto& u : units) {
                    nlohmann::json uj;
                    uj["key"] = u.key;
                    uj["inDegree"] = u.inDegree;
                    uj["topic"] = u.topic;
                    uj["isPublic"] = u.isPublic;
                    uj["isBusy"] = u.isBusy;
                    uj["turnsSinceCreation"] = u.turnsSinceCreation;
                    uj["messageCount"] = u.messageCount;
                    uj["tokenStats"] = {
                        {"totalPromptTokens", u.stats.totalPromptTokens},
                        {"totalCachedTokens", u.stats.totalCachedTokens},
                        {"totalOutputTokens", u.stats.totalOutputTokens},
                        {"lastPromptTokens", u.stats.lastPromptTokens},
                        {"lastCachedTokens", u.stats.lastCachedTokens},
                        {"lastOutputTokens", u.stats.lastOutputTokens},
                        {"lastCacheHit", u.stats.lastCacheHit},
                        {"requestCount", u.stats.requestCount}
                    };
                    j["units"].push_back(uj);
                }
                return std::make_shared<ix::HttpResponse>(200, "OK", ix::HttpErrorCode::Ok, headers, j.dump());
            }

            // GET /api/proposals/pending: 管理员专用待审阅提案列表。
            // pending 提案 MUST NOT 注入稳定 system prompt，只在此管理员通道可见；
            // approve/reject 入口在人工审核服务接入前一律返回 HUMAN_REVIEW_REQUIRED。
            if (req->uri == "/api/proposals/pending" && req->method == "GET") {
                nlohmann::json j;
                j["ok"] = true;
                j["human_review"] = "HUMAN_REVIEW_PENDING";
                j["approve_supported"] = false;  // 未接入人工审核服务
                nlohmann::json items = nlohmann::json::array();
                for (const auto& p : runtime_.pendingProposals(200)) {
                    nlohmann::json pj;
                    pj["proposal_id"] = p.proposalId;
                    pj["kind"] = mio::toString(p.kind);
                    pj["subject_id"] = p.subjectId;
                    pj["predicate"] = p.predicate;
                    pj["object"] = p.object;
                    pj["source"] = p.source;
                    pj["confidence"] = p.confidence;
                    pj["status"] = mio::toString(p.status);
                    pj["fact_status"] = mio::toString(p.factStatus);
                    pj["conversation_key"] = p.conversationKey;
                    pj["created_at"] = p.createdAt;
                    pj["review_status"] = p.review.status;
                    // 审核人/审核时间字段当前版本必须为空/0（不得伪造）
                    pj["review_actor"] = p.review.actor;
                    pj["reviewed_at"] = p.review.reviewedAt;
                    items.push_back(std::move(pj));
                }
                j["items"] = std::move(items);
                return std::make_shared<ix::HttpResponse>(200, "OK", ix::HttpErrorCode::Ok,
                                                          headers, j.dump());
            }

            // GET /api/lifecycle: 会话静默调度状态（含失败/重试耗尽的可观察状态）
            if (req->uri == "/api/lifecycle" && req->method == "GET") {
                nlohmann::json j;
                j["ok"] = true;
                const auto stats = runtime_.summaryStats();
                j["summary_stats"] = {{"succeeded", stats.succeeded},
                                      {"failed", stats.failed},
                                      {"skipped_no_content", stats.skippedNoContent}};
                nlohmann::json items = nlohmann::json::array();
                for (const auto& s : runtime_.lifecycleStates()) {
                    nlohmann::json sj;
                    sj["conversation_key"] = s.conversationKey;
                    sj["activity_version"] = s.activityVersion;
                    sj["last_activity_at"] = s.lastActivityAt;
                    sj["last_summarized_message_id"] = s.lastSummarizedMessageId;
                    sj["pending_from_message_id"] = s.pendingFromMessageId;
                    sj["pending_count"] = s.pendingCount;
                    sj["in_flight_inputs"] = s.inFlightInputs;
                    sj["active_generations"] = s.activeGenerations;
                    sj["summary_pending"] = s.summaryPending;
                    sj["summary_task_id"] = s.summaryTaskId;
                    sj["retry_count"] = s.retryCount;
                    sj["run_state"] = mio::toString(s.runState);
                    sj["last_error"] = s.lastError;
                    sj["last_attempt_at"] = s.lastAttemptAt;
                    items.push_back(std::move(sj));
                }
                j["conversations"] = std::move(items);
                return std::make_shared<ix::HttpResponse>(200, "OK", ix::HttpErrorCode::Ok,
                                                          headers, j.dump());
            }

            // POST /api/lifecycle/clear-failure: 人工排障复位（不是模型工具）
            if (req->uri == "/api/lifecycle/clear-failure" && req->method == "POST") {
                std::string convKey;
                try {
                    auto parsed = nlohmann::json::parse(req->body, nullptr, false, true);
                    if (!parsed.is_discarded() && parsed.contains("conversation_key")) {
                        convKey = parsed["conversation_key"].get<std::string>();
                    }
                } catch (...) {
                }
                if (convKey.empty()) {
                    nlohmann::json err = {{"ok", false},
                                          {"error", "缺少 conversation_key"}};
                    return std::make_shared<ix::HttpResponse>(400, "Bad Request",
                                                              ix::HttpErrorCode::Ok,
                                                              headers, err.dump());
                }
                const bool ok = runtime_.clearLifecycleFailure(convKey);
                nlohmann::json resp = {{"ok", ok},
                                       {"conversation_key", convKey},
                                       {"message", ok ? "失败状态已复位" : "未找到该会话或复位失败"}};
                return std::make_shared<ix::HttpResponse>(ok ? 200 : 404,
                                                          ok ? "OK" : "Not Found",
                                                          ix::HttpErrorCode::Ok, headers,
                                                          resp.dump());
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
