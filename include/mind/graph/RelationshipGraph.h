#pragma once
// ============================================================================
// 关系图谱（RelationshipGraph）
//
//   * 节点 = 人（【含 MIO 自己】）：name / personal(印象) / loves / attrs 等属性
//     + 各平台身份 platformIds / weight / 首次与最近遇见时间；
//   * 持久化 data/relationships.json；旧 data/people.json 首次读取自动迁移，
//     旧文件中的 edges 字段读取时直接忽略。
//
// 线程安全：公开方法内部自锁（无锁内部函数 + 加锁公开包装，避免自死锁）。
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mio {

struct SeenResult {
    bool firstEncounter = false;  // 首次遇见（供平台打初识标记）
    std::string internalId;       // 匹配到的内部 ID
};

struct PersonNode {
    std::string internalId;
    std::string name;                        // 模型选定的称呼
    std::string personal;                    // 印象描述
    std::string loves;                       // 喜好
    // 各平台身份：内容为 "[platform][id]" 复合键（platformTag 构造），
    // 作为不同平台发送者的唯一身份识别；旧数据的裸 id 查找时兼容。
    std::vector<std::string> platformIds;
    double weight = 0;                       // 权重（时间衰减累加，topKSnapshot 用）
    std::int64_t firstSeenAt = 0;
    std::int64_t lastSeenAt = 0;
    std::map<std::string, std::string> attrs;  // 可扩展属性（根据需要增删）
};

class RelationshipGraph {
public:
    explicit RelationshipGraph(std::filesystem::path file);

    // 平台身份复合键："[platform][id]"（platform 空时退化为裸 id，兼容无平台来源）
    static std::string platformTag(const std::string& platform,
                                   const std::string& id) {
        return platform.empty() ? id : "[" + platform + "][" + id + "]";
    }

    // ---- 节点 ----
    // 按 [platform][id] 匹配；首次遇见分配内部 ID 并返回标记
    SeenResult onSeen(const std::string& platform, const std::string& platformId,
                      const std::string& nameHint, std::int64_t now);
    // MIO 自身节点（启动时调用一次）
    void ensureMio(const std::string& name);

    std::optional<PersonNode> platformContact(const std::string& platform,
                                               const std::string& platformId) const;
    // 名称（无则回退 internalId）
    std::string nameOf(const std::string& internalId) const;

    // 通讯录工具使用值快照，避免离开锁后与其他会话更新同一节点发生竞争。
    std::vector<PersonNode> contacts() const;
    std::vector<PersonNode> topKSnapshot(std::size_t k) const;
    // 支持 internalId、QQ、平台复合键或唯一称呼；重名时拒绝猜测。
    PersonNode resolveContact(const std::string& person) const;
    PersonNode upsertContact(const std::string& name, const std::string& platform,
                             const std::string& platformId,
                             const std::optional<std::string>& notes,
                             std::int64_t now);
    PersonNode updateContact(const std::string& person,
                             const std::optional<std::string>& name,
                             const std::optional<std::string>& notes);

    // 辅助工具：从 PersonNode 的 platformIds 提取 QQ 号（无则返回空串）
    static std::string extractQq(const PersonNode& node);

    void load();
    void save();

private:
    // 被动路径（消息路由自动登记）的写盘失败只记日志：磁盘抖动
    // 不该打断整轮对话；显式的通讯录工具调用仍由 save() 抛异常上报给模型。
    void savePassive();

    // 无锁内部函数：调用方必须已持有 mtx_
    PersonNode* findNode(const std::string& internalId);
    const PersonNode* findNode(const std::string& internalId) const;
    const PersonNode* resolveContactLocked(const std::string& person) const;
    // 复合 tag 精确匹配；未命中且带平台时回退裸 id（旧数据兼容）
    const PersonNode* findByPlatformLocked(const std::string& platform,
                                           const std::string& platformId) const;

    static double decayed(double value, std::int64_t last, std::int64_t now);
    static void bumpWeight(PersonNode& p, std::int64_t now);

    std::filesystem::path file_;
    std::vector<std::unique_ptr<PersonNode>> nodes_;
    std::uint64_t nextInternalId_ = 1;
    std::string mioId_ = "mio";
    mutable std::mutex mtx_;

    static constexpr double kDecayTauSeconds = 3 * 24 * 3600;  // τ ≈ 3 天
};

} // namespace mio
