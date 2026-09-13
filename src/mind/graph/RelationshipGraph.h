#pragma once
// ============================================================================
// 关系图谱（RelationshipGraph）—— PeopleBook 升级版（v6）+ 子任务 C 关系状态
//
//   * 节点 = 人（【含 MIO 自己】）：name / personal(印象) / loves 等可扩展属性
//     + 原 PeopleBook 字段（platformIds / weight / 时间戳）映射进节点；
//   * 边 = 任意两人之间的关系：intimacy（亲密熟悉度，对称）、
//     trust（信任度，有向 a→b / b→a；【直接关联隐私处理政策】，当前只存数据，
//     政策后置，差异化靠装配/检索时隐藏数据实现）；
//   * 原 PeopleBook 依赖（onSeen / topK / setNickname / setNotes）全部映射到本类；
//   * 持久化 data/relationships.json；旧 data/people.json 首次读取自动迁移。
//
// 子任务 C 追加（见 docs/operations/memory-system-refactor.md「社会关系与可见性」）：
//   * 关系【标签】(relationship_type: family/friend/partner/colleague…) 与
//     【熟悉程度】(familiarity: stranger/acquaintance/familiar) 分离；
//     family 是亲属/角色关系，不是刷消息可升级的亲密度等级；
//   * blocked 是独立交互状态，不参与标签与熟悉度计算；
//   * 互动只更新熟悉/活跃统计（去重事件 + 有界增量 + 版本号），
//     不存在"每条消息 +0.1 就能接近满值"的晋级路径，
//     也【不实现】自动升为朋友/亲人或自动提高信任；
//   * 旧 personal/loves/attrs 一律标记 legacy_unverified（不自动成为 confirmed）；
//   * 迁移失败保留原文件（存储进入降级态，拒绝覆盖），迁移可重试且幂等；
//   * 写回保留不认识的旧 JSON 字段（向前兼容），保存为原子写 + 首次迁移备份。
//
// 线程安全：公开方法内部自锁（无锁内部函数 + 加锁公开包装，避免自死锁）。
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/contracts/ProposalContracts.h"
#include "mind/proposals/JsonStore.h"  // EpochClock（备份文件名用 epoch 秒）

namespace mio {

struct SeenResult {
    bool firstEncounter = false;  // 首次遇见（供平台打初识标记）
    std::string internalId;       // 匹配到的内部 ID
};

struct PersonNode {
    std::string internalId;
    std::string name;                        // 模型选定的称呼（原 nickname）
    std::string personal;                    // 印象描述（原 notes）
    std::string loves;                       // 喜好
    // 各平台身份：内容为 "[platform][id]" 复合键（platformTag 构造），
    // 作为不同平台发送者的唯一身份识别；旧数据的裸 id 查找时兼容。
    std::vector<std::string> platformIds;
    double weight = 0;                       // 权重（时间衰减累加，topK 用）
    std::int64_t firstSeenAt = 0;
    std::int64_t lastSeenAt = 0;
    std::map<std::string, std::string> attrs;  // 可扩展属性（根据需要增删）

    // ---- 子任务 C 追加 ----
    std::string relationshipType;  // "" / "family" / "friend" / "partner" / "colleague"…
    Familiarity familiarity = Familiarity::Stranger;
    bool blocked = false;          // 独立交互状态（不影响标签与熟悉度）
    bool legacyUnverified = false; // 旧图谱迁移标记：数据保留，但不作为可信关系证据
    // 关系状态最后更新时间（标签 / blocked 变更；与 lastSeenAt 分开）
    std::int64_t relationshipUpdatedAt = 0;
    // 去重后的互动事件数（活跃统计）。只用于推导 familiarity，
    // 绝不用于推导 relationship_type（family/friend 必须由人/系统显式设置）。
    std::uint64_t interactionEvents = 0;
};

// 两人之间的关系边；a<b 归一（无向边记录，trust 分两个方向）
struct RelationshipEdge {
    std::string a;
    std::string b;
    double intimacy = 0;   // 亲密熟悉度（对称）
    double trustAB = 0;    // a 信任 b
    double trustBA = 0;    // b 信任 a

    // ---- 子任务 C 追加：事件来源 / 去重 / 版本 ----
    std::int64_t lastInteractionAt = 0;
    std::uint64_t interactionCount = 0;      // 去重后累计事件数
    std::uint64_t version = 0;               // 每次实际变更 +1（可审计）
    std::vector<std::string> eventKeys;      // 去重键（有界：仅保留最近 N 个）
};

class RelationshipGraph {
public:
    // 熟悉度阈值：按【去重后的互动事件数】推导，永远不产生 family/friend
    static constexpr std::uint64_t kAcquaintanceEvents = 3;
    static constexpr std::uint64_t kFamiliarEvents = 12;
    // 单次事件允许的亲密增量上限（防止任意覆盖 / 刷分）
    static constexpr double kMaxEventDelta = 0.1;
    static constexpr double kDefaultEventDelta = 0.05;
    // 事件去重键保留数量（有界内存；跨重启仍能挡住重复上报）
    static constexpr std::size_t kMaxEventKeys = 64;
    // 旧 weight 保守推断 acquaintance 的下限（永不超过 acquaintance）
    static constexpr double kLegacyAcquaintanceWeight = 5.0;

    // clock 只用于迁移备份文件名（epoch 秒）与未来时间戳；默认系统时钟
    explicit RelationshipGraph(std::filesystem::path file,
                               EpochClock clock = systemEpochClock());

    // 平台身份复合键："[platform][id]"（platform 空时退化为裸 id，兼容无平台来源）
    static std::string platformTag(const std::string& platform,
                                   const std::string& id) {
        return platform.empty() ? id : "[" + platform + "][" + id + "]";
    }

    // ---- 节点（原 PeopleBook API 映射）----
    // 按 [platform][id] 匹配；首次遇见分配内部 ID 并返回标记
    SeenResult onSeen(const std::string& platform, const std::string& platformId,
                      const std::string& nameHint, std::int64_t now);
    // MIO 自身节点（启动时调用一次）
    void ensureMio(const std::string& name);

    // 模型工具（只改称呼/印象，不改映射）；按名称查找
    bool setNickname(const std::string& currentName, const std::string& newName);
    bool setNotes(const std::string& name, const std::string& notes);
    bool setPersonal(const std::string& name, const std::string& personal);
    bool setLoves(const std::string& name, const std::string& loves);

    // 名称精确查找。重名（歧义）时返回 nullptr —— 宁可失败也不绑错身份；
    // 需要区分"不存在"与"歧义"时用 findByNameAll / findByNameUnique。
    const PersonNode* findByName(const std::string& name) const;
    // 同名全部节点（歧义检测用）
    std::vector<const PersonNode*> findByNameAll(const std::string& name) const;
    // 唯一名称查找：歧义返回 false（internalId 不写出）
    bool findByNameUnique(const std::string& name, std::string* internalId) const;
    // 按 [platform][id] 匹配；旧数据的裸 id 回退兼容
    const PersonNode* findByPlatform(const std::string& platform,
                                     const std::string& platformId) const;
    const PersonNode* findById(const std::string& internalId) const;
    // 名称（无则回退 internalId）；Fusion 标签等用
    std::string nameOf(const std::string& internalId) const;
    // Facts 渲染用：按权重降序取前 K（同权重按 internalId 升序，保证确定性输出；
    // 返回的指针在下次写操作前有效）
    std::vector<const PersonNode*> topK(std::size_t k) const;
    // 获取除 MIO 外所有已记录的认识的人（工具查询等用）
    std::vector<const PersonNode*> allPersons() const;

    // 辅助工具：从 PersonNode 的 platformIds 提取 QQ 号（无则返回空串）
    static std::string extractQq(const PersonNode& node);

    // ---- 边 ----
    // 互动统计入口（子任务 F 调用）：
    //   * 只更新熟悉度/活跃统计，不做关系标签晋级；
    //   * 按 eventSource 去重（同一次事件重复上报不重复计分）；
    //   * 增量有界，intimacy 恒在 [0,1]；每次实际变更 version +1。
    void noteInteraction(const std::string& a, const std::string& b,
                         const std::string& eventSource, std::int64_t now);

    // 旧签名兼容：无事件来源的调用按 "legacy:<pair>:<now>" 合成来源，
    // 即同一秒内同一对人只计一次（幂等），仍然有上下界与版本。
    void bumpIntimacy(const std::string& a, const std::string& b, double delta,
                      std::int64_t now);
    // 带事件来源的增量入口（事件来源去重 + 上下界 + 版本）
    void bumpIntimacy(const std::string& a, const std::string& b, double delta,
                      std::int64_t now, const std::string& eventSource);

    double intimacy(const std::string& a, const std::string& b) const;
    void setTrust(const std::string& a, const std::string& b, double value);
    double trust(const std::string& a, const std::string& b) const;

    // 去重后的互动事件数（统计/测试用）
    std::uint64_t interactionCount(const std::string& a,
                                   const std::string& b) const;

    // ---- 关系状态（子任务 C）----
    // 关系【标签】只能由自动规则或未来人工审核服务提交状态变更；普通工具
    // 只能提交 pending 提案（ProposalStore），不得调用本入口。
    // source 为 "model*" 一律拒绝；family/partner 只接受可信来源。
    bool setRelationshipType(const std::string& personId,
                             const std::string& relationshipType,
                             const std::string& source, std::int64_t now,
                             std::string* error);
    bool setBlocked(const std::string& personId, bool blocked, std::int64_t now);
    Familiarity familiarityOf(const std::string& personId) const;
    // 关系状态快照（IRelationshipStore::get 的实现基础）
    RelationshipState relationshipState(const std::string& personId) const;

    std::string mioId() const;

    // ---- 持久化 ----
    void load();
    void save();
    // 迁移可重试：重新读取磁盘文件（用于损坏文件修复后恢复）
    bool reload();
    // 加载失败 → 降级：save() 拒绝覆盖原文件，避免破坏用户数据
    bool storageDegraded() const;
    std::string lastLoadError() const;

private:
    // 无锁内部函数：调用方必须已持有 mtx_
    PersonNode* findNode(const std::string& internalId);
    const PersonNode* findNode(const std::string& internalId) const;
    const PersonNode* findByNameLocked(const std::string& name) const;
    std::vector<const PersonNode*> findByNameAllLocked(
        const std::string& name) const;
    // 复合 tag 精确匹配；未命中且带平台时回退裸 id（旧数据兼容）
    const PersonNode* findByPlatformLocked(const std::string& platform,
                                           const std::string& platformId) const;
    RelationshipEdge* ensureEdge(const std::string& a, const std::string& b);
    RelationshipEdge* findEdge(const std::string& a, const std::string& b) const;
    // 事件增量实现（去重 + 有界 + 版本）；返回是否实际变更
    bool applyEventLocked(const std::string& a, const std::string& b,
                          double delta, const std::string& eventSource,
                          std::int64_t now);
    // 由去重事件数推导熟悉度（只会升，不会降；永不产生 family/friend）
    void refreshFamiliarityLocked(PersonNode& node);
    void loadLocked();
    bool saveLocked();

    static double decayed(double value, std::int64_t last, std::int64_t now);
    static void bumpWeight(PersonNode& p, std::int64_t now);
    static bool isValidRelationshipType(const std::string& type);
    static bool isTrustedRelationshipSource(const std::string& source);

    std::filesystem::path file_;
    EpochClock clock_;
    std::vector<std::unique_ptr<PersonNode>> nodes_;
    std::vector<std::unique_ptr<RelationshipEdge>> edges_;
    std::uint64_t nextInternalId_ = 1;
    std::string mioId_ = "mio";
    mutable std::mutex mtx_;

    // 向前兼容：保留不认识的旧 JSON 字段，写回不丢
    nlohmann::json rootExtra_ = nlohmann::json::object();
    std::map<std::string, nlohmann::json> nodeExtra_;
    std::map<std::string, nlohmann::json> edgeExtra_;
    // 降级标记：加载/迁移失败后拒绝覆盖原文件
    bool degraded_ = false;
    std::string lastLoadError_;

    static constexpr double kDecayTauSeconds = 3 * 24 * 3600;  // τ ≈ 3 天
};

} // namespace mio
