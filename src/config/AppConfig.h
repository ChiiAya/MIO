#pragma once

#include "adapters/napcat/NapCatAdapter.h"
#include "config/embedding/EmbeddingConfig.h"
#include "config/openai/OpenaiConfig.h"
#include "context/contextBuilder/ContextBuilder.h"
#include "context/conversationFusion/FusionRouter.h"
#include "memory/manager/MemoryManager.h"

#include <nlohmann/json.hpp>
#include <string>

namespace mio {

struct AppConfig {
    std::string botName = "Mio";
    std::string dataDir = "data";
    std::string platform = "console";
    std::string llmBackend = "openai";

    OpenAiConfig openai;
    EmbeddingConfig embedding;
    FusionConfig fusion;
    ContextBuilderConfig contextBuilder;
    MemoryConfig memory;
    NapCatConfig napcat;

    static AppConfig fromEnvironment();
};

// nlohmann::json 序列化与反序列化（支持局部更新/不完全覆盖）
void to_json(nlohmann::json& j, const OpenAiConfig& c);
void from_json(const nlohmann::json& j, OpenAiConfig& c);

void to_json(nlohmann::json& j, const EmbeddingConfig& c);
void from_json(const nlohmann::json& j, EmbeddingConfig& c);

void to_json(nlohmann::json& j, const FusionConfig& c);
void from_json(const nlohmann::json& j, FusionConfig& c);

void to_json(nlohmann::json& j, const ContextBuilderConfig& c);
void from_json(const nlohmann::json& j, ContextBuilderConfig& c);

void to_json(nlohmann::json& j, const MemoryConfig& c);
void from_json(const nlohmann::json& j, MemoryConfig& c);

void to_json(nlohmann::json& j, const NapCatConfig& c);
void from_json(const nlohmann::json& j, NapCatConfig& c);

void to_json(nlohmann::json& j, const AppConfig& c);
void from_json(const nlohmann::json& j, AppConfig& c);

} // namespace mio

