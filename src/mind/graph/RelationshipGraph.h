#pragma once
// ============================================================================
// 关系图谱（RelationshipGraph）—— PeopleBook 升级版（v6）
//
//   * 节点 = 人（【含 MIO 自己】）：name / personal(印象) / loves 等可扩展属性
//     + 原 PeopleBook 字段（platformIds / weight / 时间戳）映射进节点；
//   * 边 = 任意两人之间的关系：intimacy（亲密熟悉度，对称）、
//     trust（信任度，有向 a→b / b→a；【直接关联隐私处理政策】，当前只存数据，
//     政策后置，差异化靠装配/检索时隐藏数据实现）；
//   * 原 PeopleBook 依赖（onSeen / topK / setNickname / setNotes）全部映射到本类；
//   * 持久化 data/relationships.json；旧 data/people.json 首次读取自动迁移。
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
};

// 两人之间的关系边；a<b 归一（无向边记录，trust 分两个方向）
struct RelationshipEdge {
    std::string a;
    std::string b;
    double intimacy = 0;   // 亲密熟悉度（对称）
    double trustAB = 0;    // a 信任 b
    double trustBA = 0;    // b 信任 a
};

class RelationshipGraph {
public:
    explicit RelationshipGraph(std::filesystem::path file);

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

    const PersonNode* findByName(const std::string& name) const;
    // 按 [platform][id] 匹配；旧数据的裸 id 回退兼容
    const PersonNode* findByPlatform(const std::string& platform,
                                     const std::string& platformId) const;
    const PersonNode* findById(const std::string& internalId) const;
    // 名称（无则回退 internalId）；Fusion 标签等用
    std::string nameOf(const std::string& internalId) const;
    // Facts 渲染用：按权重降序取前 K（返回的指针在下次写操作前有效）
    std::vector<const PersonNode*> topK(std::size_t k) const;

    // ---- 边 ----
    // 亲密熟悉度（时间衰减 + 增量，上限 1.0；无则创建）
    void bumpIntimacy(const std::string& a, const std::string& b,
                      double delta, std::int64_t now);
    double intimacy(const std::string& a, const std::string& b) const;
    void setTrust(const std::string& a, const std::string& b, double value);
    double trust(const std::string& a, const std::string& b) const;

    std::string mioId() const;

    void load();
    void save();

private:
    // 无锁内部函数：调用方必须已持有 mtx_
    PersonNode* findNode(const std::string& internalId);
    const PersonNode* findNode(const std::string& internalId) const;
    const PersonNode* findByNameLocked(const std::string& name) const;
    // 复合 tag 精确匹配；未命中且带平台时回退裸 id（旧数据兼容）
    const PersonNode* findByPlatformLocked(const std::string& platform,
                                           const std::string& platformId) const;
    RelationshipEdge* ensureEdge(const std::string& a, const std::string& b);
    RelationshipEdge* findEdge(const std::string& a, const std::string& b) const;

    static double decayed(double value, std::int64_t last, std::int64_t now);
    static void bumpWeight(PersonNode& p, std::int64_t now);

    std::filesystem::path file_;
    std::vector<std::unique_ptr<PersonNode>> nodes_;
    std::vector<std::unique_ptr<RelationshipEdge>> edges_;
    std::uint64_t nextInternalId_ = 1;
    std::string mioId_ = "mio";
    mutable std::mutex mtx_;

    static constexpr double kDecayTauSeconds = 3 * 24 * 3600;  // τ ≈ 3 天
};

} // namespace mio
