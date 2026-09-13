#pragma once

#include "config/AppConfig.h"

#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>

namespace mio {

class ConfigManager {
public:
    explicit ConfigManager(std::shared_ptr<const AppConfig> initialConfig = nullptr);

    // 线程安全获取当前不可变配置快照
    std::shared_ptr<const AppConfig> get() const;

    // 根据 JSON 文本局部更新配置并发布新快照
    bool reload(const std::string& jsonString);

    // 从文件加载配置并发布新快照；文件不存在时若为默认路径则平滑回退至环境变量
    bool reloadFromFile(const std::filesystem::path& path = "config.json");

    // 解析 + 校验候选配置但【不发布】：以当前配置为基底做局部更新。
    // 供调用方先构造并校验候选配置与依赖，再决定是否发布（失败不部分套用）。
    bool loadCandidateFromFile(const std::filesystem::path& path, AppConfig& out,
                               std::string* error) const;

    // 从环境变量重新加载并发布新快照
    bool reloadFromEnvironment();

    // 手动发布新配置快照
    void set(std::shared_ptr<const AppConfig> newConfig);

private:
    std::shared_ptr<const AppConfig> current_;
    mutable std::shared_mutex mtx_;
};

} // namespace mio

