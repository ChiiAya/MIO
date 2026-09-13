#pragma once
// ============================================================================
// 可见性契约（共享，冻结）
//
// 规则（见 docs/operations/memory-system-refactor.md）：
//   * 记忆默认 conversation 可见；
//   * 摘要输出仅可【建议收窄】，不能自动扩大为 public；
//   * 未知或解析失败保持最窄范围；
//   * 亲密度 / trust / 摘要模型的"公开"判断都不能授权跨会话访问。
//
// 顺序即宽窄：Conversation < Person < Public。narrower()/widest() 依赖该顺序，
// 不得重排。
// ============================================================================

#include <string>

namespace mio {

enum class Visibility {
    Conversation = 0,  // 仅产生该记忆的物理会话可见（默认，最窄）
    Person = 1,        // 归属人跨会话可见（私聊归属人；群聊无归属人时不使用）
    Public = 2,        // 显式公开（当前阶段仅系统/人工可写，模型不可提升）
};

// 取更窄的一侧（用于"只可收窄"的合并语义）
inline Visibility narrower(Visibility a, Visibility b) {
    return static_cast<int>(a) <= static_cast<int>(b) ? a : b;
}

// 取更宽的一侧（仅用于系统显式授予的场景，例如系统初始化的事实）
inline Visibility wider(Visibility a, Visibility b) {
    return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}

inline const char* toString(Visibility v) {
    switch (v) {
    case Visibility::Conversation: return "conversation";
    case Visibility::Person: return "person";
    case Visibility::Public: return "public";
    }
    return "conversation";
}

// 严格解析：未知值返回 false，调用方必须保持最窄范围（不得默认放宽）
inline bool parseVisibility(const std::string& s, Visibility& out) {
    if (s == "conversation") { out = Visibility::Conversation; return true; }
    if (s == "person") { out = Visibility::Person; return true; }
    if (s == "public") { out = Visibility::Public; return true; }
    return false;
}

// 宽松解析：失败时返回最窄范围（旧数据 isPublic 字段迁移用）
inline Visibility parseVisibilityOrNarrowest(const std::string& s) {
    Visibility v = Visibility::Conversation;
    parseVisibility(s, v);
    return v;
}

} // namespace mio
