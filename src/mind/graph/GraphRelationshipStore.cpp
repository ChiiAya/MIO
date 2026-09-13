#include "mind/graph/GraphRelationshipStore.h"

#include "log/Log.h"

namespace mio {

GraphRelationshipStore::GraphRelationshipStore(RelationshipGraph& graph)
    : graph_(graph) {}

RelationshipState GraphRelationshipStore::get(
    const std::string& personId) const {
    return graph_.relationshipState(personId);
}

bool GraphRelationshipStore::setRelationshipType(
    const std::string& personId, const std::string& relationshipType,
    const std::string& source, std::int64_t now, std::string* error) {
    // 关系类别不是模型可写字段：本入口由自动规则 / 未来人工审核服务调用
    return graph_.setRelationshipType(personId, relationshipType, source, now,
                                      error);
}

void GraphRelationshipStore::bumpIntimacy(const std::string& personId,
                                          double delta,
                                          const std::string& eventSource,
                                          std::int64_t now) {
    if (eventSource.empty()) {
        log::warn("GraphRelationshipStore",
                  "亲密度事件缺少来源，无法去重；调用方应提供 conv#messageId 形式来源");
    }
    // 亲密度是"我与他"的边：事件增量 + 去重 + 上下界
    graph_.bumpIntimacy(graph_.mioId(), personId, delta, now, eventSource);
}

void GraphRelationshipStore::setBlocked(const std::string& personId,
                                        bool blocked, std::int64_t now) {
    if (!graph_.setBlocked(personId, blocked, now)) {
        log::warn("GraphRelationshipStore",
                  "setBlocked 失败：人物不存在 " + personId);
    }
}

} // namespace mio
