#pragma once
// ============================================================================
// GraphRelationshipStore —— RelationshipGraph 的 IRelationshipStore 适配器（子任务 C）
//
//   * get(personId) 返回 RelationshipState（未知身份 = 首次接触前的 stranger）；
//   * setRelationshipType 只允许自动规则 / 未来人工审核调用，普通模型工具
//     只能提交 pending 提案（ProposalStore）——本适配器【不得】注册成模型工具；
//   * bumpIntimacy 走事件增量（来源去重 + 上下界 + 版本），不做分数晋级；
//   * setBlocked 只改独立交互状态。
//
// 说明：get() 的 intimacyScore / trustScore 取 MIO↔personId 这条边（即"我与他
// 的关系"）；person↔person 的边仍由图谱内部事件维护，不对外当作关系类别。
// ============================================================================

#include "core/contracts/ProposalContracts.h"
#include "mind/graph/RelationshipGraph.h"

namespace mio {

class GraphRelationshipStore : public IRelationshipStore {
public:
    explicit GraphRelationshipStore(RelationshipGraph& graph);

    RelationshipState get(const std::string& personId) const override;

    bool setRelationshipType(const std::string& personId,
                             const std::string& relationshipType,
                             const std::string& source, std::int64_t now,
                             std::string* error) override;

    void bumpIntimacy(const std::string& personId, double delta,
                      const std::string& eventSource, std::int64_t now) override;

    void setBlocked(const std::string& personId, bool blocked,
                    std::int64_t now) override;

private:
    RelationshipGraph& graph_;
};

} // namespace mio
