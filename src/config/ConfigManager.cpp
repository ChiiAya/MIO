#include "config/ConfigManager.h"
#include "log/Log.h"

#include <fstream>
#include <sstream>

namespace mio {

ConfigManager::ConfigManager(std::shared_ptr<const AppConfig> initialConfig) {
    if (initialConfig) {
        current_ = std::move(initialConfig);
    } else if (std::getenv("MIO_NO_CONFIG") != nullptr) {
        current_ = std::make_shared<const AppConfig>(AppConfig::fromEnvironment());
    } else if (const char* envPath = std::getenv("MIO_CONFIG_PATH"); envPath && std::filesystem::exists(envPath)) {
        if (!reloadFromFile(envPath)) {
            current_ = std::make_shared<const AppConfig>(AppConfig::fromEnvironment());
        }
    } else if (std::filesystem::exists("config.json")) {
        if (!reloadFromFile("config.json")) {
            current_ = std::make_shared<const AppConfig>(AppConfig::fromEnvironment());
        }
    } else {
        current_ = std::make_shared<const AppConfig>(AppConfig::fromEnvironment());
    }
}

ConfigManager::ConfigManager(const std::filesystem::path& configPath) {
    if (!configPath.empty() && std::filesystem::exists(configPath)) {
        if (!reloadFromFile(configPath)) {
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

    {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        current_ = std::make_shared<const AppConfig>(std::move(updated));
    }
    return true;
}

bool ConfigManager::reloadFromFile(const std::filesystem::path& path) {
    if (path == "config.json" && std::getenv("MIO_NO_CONFIG") != nullptr) {
        log::info("ConfigManager", "MIO_NO_CONFIG 启用，从环境变量重新加载");
        return reloadFromEnvironment();
    }

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
    AppConfig envCfg = AppConfig::fromEnvironment();
    {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        current_ = std::make_shared<const AppConfig>(std::move(envCfg));
    }
    return true;
}

void ConfigManager::set(std::shared_ptr<const AppConfig> newConfig) {
    if (!newConfig) return;
    std::unique_lock<std::shared_mutex> lock(mtx_);
    current_ = std::move(newConfig);
}

} // namespace mio

