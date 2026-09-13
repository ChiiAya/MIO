#include "config/ConfigManager.h"
#include "log/Log.h"

#include <fstream>
#include <sstream>

namespace mio {

ConfigManager::ConfigManager(std::shared_ptr<const AppConfig> initialConfig) {
    if (initialConfig) {
        current_ = std::move(initialConfig);
    } else if (std::getenv("MIO_NO_CONFIG") == nullptr && std::filesystem::exists("config.json")) {
        if (!reloadFromFile("config.json")) {
            current_ = std::make_shared<const AppConfig>(AppConfig::fromEnvironment());
        }
    } else {
        current_ = std::make_shared<const AppConfig>(AppConfig::fromEnvironment());
    }
}

std::shared_ptr<const AppConfig> ConfigManager::get() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    return current_;
}

bool ConfigManager::reload(const std::string& jsonString) {
    nlohmann::json j = nlohmann::json::parse(jsonString, nullptr, false, true);
    if (j.is_discarded()) {
        log::warn("ConfigManager", "JSON 解析失败，保持当前配置");
        return false;
    }

    AppConfig updated;
    {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        if (current_) {
            updated = *current_;
        } else {
            updated = AppConfig::fromEnvironment();
        }
    }

    try {
        from_json(j, updated);
    } catch (const std::exception& e) {
        log::warn("ConfigManager", std::string("配置反序列化失败: ") + e.what());
        return false;
    }

    // 先构造并校验候选配置，再切换运行状态：
    // 新字段类型或范围错误时整次重载失败，不发布任何部分配置。
    std::string validationError;
    if (!updated.validate(&validationError)) {
        log::warn("ConfigManager", "配置校验失败，保持当前配置: " + validationError);
        return false;
    }

    {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        current_ = std::make_shared<const AppConfig>(std::move(updated));
    }
    return true;
}

bool ConfigManager::loadCandidateFromFile(const std::filesystem::path& path,
                                          AppConfig& out, std::string* error) const {
    auto fail = [error](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };

    // 以当前配置为基底：缺失字段保持原值（局部更新语义）
    {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        out = current_ ? *current_ : AppConfig::fromEnvironment();
    }

    if (std::filesystem::exists(path)) {
        std::ifstream file(path);
        if (!file.is_open()) return fail("无法打开配置文件: " + path.string());
        std::stringstream ss;
        ss << file.rdbuf();
        nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false, true);
        if (j.is_discarded()) return fail("JSON 解析失败: " + path.string());
        try {
            from_json(j, out);
        } catch (const std::exception& e) {
            return fail(std::string("配置反序列化失败: ") + e.what());
        }
    } else if (path == "config.json") {
        // 默认路径缺失：按"仅环境变量覆盖"处理，不重置其他字段
        std::string envError;
        if (!out.applyEnvironment(&envError)) return fail(envError);
    } else {
        return fail("配置文件不存在: " + path.string());
    }

    std::string validationError;
    if (!out.validate(&validationError)) return fail(validationError);
    if (error) error->clear();
    return true;
}

bool ConfigManager::reloadFromFile(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        if (path == "config.json") {
            log::info("ConfigManager", "未找到 config.json，从环境变量重新加载");
            return reloadFromEnvironment();
        }
        log::warn("ConfigManager", "配置文件不存在: " + path.string());
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        log::warn("ConfigManager", "无法打开配置文件: " + path.string());
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    return reload(ss.str());
}

bool ConfigManager::reloadFromEnvironment() {
    // 环境变量重载仅覆盖【实际存在】的变量，不重置其他字段：
    // 以当前配置为基底叠加，缺失字段保持原值。
    AppConfig updated;
    {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        if (current_) {
            updated = *current_;
        } else {
            updated = AppConfig::fromEnvironment();
        }
    }

    std::string error;
    if (!updated.applyEnvironment(&error)) {
        log::warn("ConfigManager", "环境变量配置非法，保持当前配置: " + error);
        return false;
    }
    if (!updated.validate(&error)) {
        log::warn("ConfigManager", "配置校验失败，保持当前配置: " + error);
        return false;
    }

    {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        current_ = std::make_shared<const AppConfig>(std::move(updated));
    }
    return true;
}

void ConfigManager::set(std::shared_ptr<const AppConfig> newConfig) {
    if (!newConfig) return;
    std::unique_lock<std::shared_mutex> lock(mtx_);
    current_ = std::move(newConfig);
}

} // namespace mio

