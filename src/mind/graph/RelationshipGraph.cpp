// ============================================================================
// 关系图谱实现（见 mind/graph/RelationshipGraph.h）
// 文件格式 data/relationships.json（子任务 C 起带 schema/version）：
//   { "schema": "mio.relationship_graph", "version": 2,
//     "nextId": 6, "mioId": "mio",
//     "nodes": [ {internalId,name,personal,loves,platformIds,weight,firstSeenAt,
//                 lastSeenAt,attrs,relationship_type,familiarity,blocked,
//                 legacy_unverified,interaction_events,relationship_updated_at} ],
//     "edges": [ {a,b,intimacy,trustAB,trustBA,last_interaction_at,
//                 interaction_count,version,event_keys} ] }
// 旧版 people.json / 旧版 relationships.json（无 schema）首次读取时迁移：
//   * 先备份 relationships.json.bak-<epoch>，再原子改写为新格式；
//   * 旧 personal/loves/attrs 标记 legacy_unverified，不自动成为 confirmed 事实；
//   * 旧分数只更新活跃统计，绝不推断 family/friend；
//   * 解析失败保留原文件不动（进入降级态，拒绝覆盖），可用 reload() 重试。
// 写回保留不认识的旧 JSON 字段（向前兼容）。
// ============================================================================

#include "mind/graph/RelationshipGraph.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>
#include "log/Log.h"

namespace mio {
namespace {

constexpr const char* kSchema = "mio.relationship_graph";
constexpr int kSchemaVersion = 2;

const std::set<std::string>& managedNodeKeys() {
    static const std::set<std::string> keys = {
        "internalId",      "name",          "personal",
        "loves",           "platformIds",   "weight",
        "firstSeenAt",     "lastSeenAt",    "attrs",
        "relationship_type", "familiarity", "blocked",
        "legacy_unverified", "interaction_events",
        "relationship_updated_at"};
    return keys;
}

const std::set<std::string>& managedEdgeKeys() {
    static const std::set<std::string> keys = {
        "a", "b", "intimacy", "trustAB", "trustBA",
        "last_interaction_at", "interaction_count", "version", "event_keys"};
    return keys;
}

std::string edgeKey(const std::string& a, const std::string& b) {
    return (a < b ? a : b) + "|" + (a < b ? b : a);
}

bool regexLikeSimpleType(const std::string& type) {
    if (type.empty() || type.size() > 32) return false;
    for (const char c : type) {
        if (!((c >= 'a' && c <= 'z') || c == '_')) return false;
    }
    return true;
}

// 未知字段收集：保证容器一定是 object（避免 nlohmann 空值语义踩坑）
void putExtra(std::map<std::string, nlohmann::json>& m, const std::string& key,
              const std::string& field, const nlohmann::json& value) {
    nlohmann::json& obj = m[key];
    if (!obj.is_object()) obj = nlohmann::json::object();
    obj[field] = value;
}

} // namespace

RelationshipGraph::RelationshipGraph(std::filesystem::path file, EpochClock clock)
    : file_(std::move(file)), clock_(std::move(clock)) {
    load();  // 构造后无并发，load 内部自锁（含迁移写回）
}

double RelationshipGraph::decayed(double value, std::int64_t last,
                                  std::int64_t now) {
    if (last <= 0 || now <= last) return value;
    const double dt = static_cast<double>(now - last);
    return value * std::exp(-dt / kDecayTauSeconds);
}

void RelationshipGraph::bumpWeight(PersonNode& p, std::int64_t now) {
    p.weight = decayed(p.weight, p.lastSeenAt, now) + 1.0;
    p.lastSeenAt = now;
}

PersonNode* RelationshipGraph::findNode(const std::string& internalId) {
    for (const auto& n : nodes_)
        if (n->internalId == internalId) return n.get();
    return nullptr;
}

const PersonNode* RelationshipGraph::findNode(
    const std::string& internalId) const {
    for (const auto& n : nodes_)
        if (n->internalId == internalId) return n.get();
    return nullptr;
}

const PersonNode* RelationshipGraph::findByNameLocked(
    const std::string& name) const {
    for (const auto& n : nodes_)
        if (n->name == name) return n.get();
    return nullptr;
}

std::vector<const PersonNode*> RelationshipGraph::findByNameAllLocked(
    const std::string& name) const {
    std::vector<const PersonNode*> out;
    if (name.empty()) return out;
    for (const auto& n : nodes_)
        if (n->name == name) out.push_back(n.get());
    return out;
}

const PersonNode* RelationshipGraph::findByPlatformLocked(
    const std::string& platform, const std::string& platformId) const {
    // 先按 "[platform][id]" 精确匹配；未命中且带平台时回退裸 id（旧数据）
    const std::string tag = platformTag(platform, platformId);
    for (const auto& n : nodes_)
        for (const auto& pid : n->platformIds)
            if (pid == tag) return n.get();
    if (!platform.empty()) {
        for (const auto& n : nodes_)
            for (const auto& pid : n->platformIds)
                if (pid == platformId) return n.get();
    }
    return nullptr;
}

RelationshipEdge* RelationshipGraph::findEdge(
    const std::string& a, const std::string& b) const {
    const std::string lo = a < b ? a : b;
    const std::string hi = a < b ? b : a;
    for (const auto& e : edges_)
        if (e->a == lo && e->b == hi) return e.get();
    return nullptr;
}

RelationshipEdge* RelationshipGraph::ensureEdge(const std::string& a,
                                                const std::string& b) {
    if (RelationshipEdge* e = findEdge(a, b); e != nullptr) return e;
    auto e = std::make_unique<RelationshipEdge>();
    const std::string lo = a < b ? a : b;
    const std::string hi = a < b ? b : a;
    e->a = lo;
    e->b = hi;
    e->trustAB = 0.5;  // 默认中性信任
    e->trustBA = 0.5;
    edges_.push_back(std::move(e));
    return edges_.back().get();
}

void RelationshipGraph::refreshFamiliarityLocked(PersonNode& node) {
    // 只按【去重后的互动事件数】提升熟悉度，且永不产生 family/friend；
    // 长期未联系不降级（不得把沉默解释为关系失效）。
    Familiarity derived = Familiarity::Stranger;
    if (node.interactionEvents >= kFamiliarEvents) {
        derived = Familiarity::Familiar;
    } else if (node.interactionEvents >= kAcquaintanceEvents) {
        derived = Familiarity::Acquaintance;
    }
    if (static_cast<int>(derived) > static_cast<int>(node.familiarity)) {
        node.familiarity = derived;
    }
}

bool RelationshipGraph::applyEventLocked(const std::string& a,
                                         const std::string& b, double delta,
                                         const std::string& eventSource,
                                         std::int64_t now) {
    if (a == b || a.empty() || b.empty()) return false;
    PersonNode* na = findNode(a);
    PersonNode* nb = findNode(b);
    if (na == nullptr || nb == nullptr) {
        // 禁止凭空发明身份：未知 ID 不参与互动计分
        log::warn("RelationshipGraph",
                  "noteInteraction 跳过未知身份: " + a + " / " + b);
        return false;
    }
    if (!(delta > 0.0)) {
        log::warn("RelationshipGraph", "忽略非正增量（不得任意覆盖亲密度）");
        return false;
    }
    const double bounded = std::min(delta, kMaxEventDelta);
    RelationshipEdge* e = ensureEdge(a, b);
    // 事件来源：空来源用 pair+秒 合成（同一秒同一对人只计一次）
    std::string source = eventSource;
    if (source.empty())
        source = "legacy:" + edgeKey(a, b) + ":" + std::to_string(now);
    if (std::find(e->eventKeys.begin(), e->eventKeys.end(), source) !=
        e->eventKeys.end()) {
        return false;  // 重复上报：不重复计分
    }
    e->intimacy = std::min(1.0, std::max(0.0,
                                         decayed(e->intimacy,
                                                 e->lastInteractionAt, now) +
                                             bounded));
    e->lastInteractionAt = now;
    ++e->interactionCount;
    ++e->version;
    e->eventKeys.push_back(std::move(source));
    if (e->eventKeys.size() > kMaxEventKeys)
        e->eventKeys.erase(e->eventKeys.begin());

    ++na->interactionEvents;
    ++nb->interactionEvents;
    refreshFamiliarityLocked(*na);
    refreshFamiliarityLocked(*nb);
    return true;
}

void RelationshipGraph::noteInteraction(const std::string& a,
                                        const std::string& b,
                                        const std::string& eventSource,
                                        std::int64_t now) {
    if (eventSource.empty()) {
        log::warn("RelationshipGraph",
                  "noteInteraction 需要事件来源以去重；本次按 pair+秒 合成来源");
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (applyEventLocked(a, b, kDefaultEventDelta, eventSource, now)) {
        if (!saveLocked())
            log::error("RelationshipGraph", "互动统计落盘失败（内存已更新）");
    }
}

void RelationshipGraph::bumpIntimacy(const std::string& a, const std::string& b,
                                     double delta, std::int64_t now) {
    // 兼容旧签名：无事件来源 → 合成 "legacy:<pair>:<now>"，同一秒幂等
    bumpIntimacy(a, b, delta, now, "");
}

void RelationshipGraph::bumpIntimacy(const std::string& a, const std::string& b,
                                     double delta, std::int64_t now,
                                     const std::string& eventSource) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (applyEventLocked(a, b, delta, eventSource, now)) {
        if (!saveLocked())
            log::error("RelationshipGraph", "亲密度落盘失败（内存已更新）");
    }
}

SeenResult RelationshipGraph::onSeen(const std::string& platform,
                                     const std::string& platformId,
                                     const std::string& nameHint,
                                     std::int64_t now) {
    std::lock_guard<std::mutex> lock(mtx_);
    SeenResult result;
    if (const PersonNode* p = findByPlatformLocked(platform, platformId);
        p != nullptr) {
        result.firstEncounter = false;
        result.internalId = p->internalId;
        PersonNode* n = findNode(p->internalId);
        // 旧数据迁移：裸 id 命中 → 原地升级为 "[platform][id]" 复合键
        const std::string tag = platformTag(platform, platformId);
        if (!platform.empty())
            for (auto& pid : n->platformIds)
                if (pid == platformId) pid = tag;
        if (n->name == platformId && !nameHint.empty() && nameHint != platformId) {
            n->name = nameHint;
        }
        const std::int64_t prevLast = n->lastSeenAt;
        bumpWeight(*n, now);
        const std::string id = n->internalId;
        // 互动事件：按 "seen:<tag>:<now>" 去重（同秒重复不计分）；
        // 只更新熟悉/活跃统计与有界增量，不做任何关系类别晋级。
        const std::string source =
            "seen:" + tag + ":" + std::to_string(now);
        RelationshipEdge* e = ensureEdge(mioId_, id);
        if (std::find(e->eventKeys.begin(), e->eventKeys.end(), source) ==
            e->eventKeys.end()) {
            e->intimacy = std::min(
                1.0, std::max(0.0,
                              decayed(e->intimacy, prevLast, now) +
                                  kDefaultEventDelta));
            e->lastInteractionAt = now;
            ++e->interactionCount;
            ++e->version;
            e->eventKeys.push_back(source);
            if (e->eventKeys.size() > kMaxEventKeys)
                e->eventKeys.erase(e->eventKeys.begin());
            ++n->interactionEvents;
            refreshFamiliarityLocked(*n);
        }
        if (!saveLocked())
            log::error("RelationshipGraph", "onSeen 落盘失败（内存已更新）");
        return result;
    }

    // 首次遇见：分配内部 ID，建立节点 + MIO↔此人 边（stranger，亲密度 0）
    auto node = std::make_unique<PersonNode>();
    node->internalId = "p" + std::to_string(nextInternalId_++);
    node->name = nameHint.empty() ? platformId : nameHint;
    node->platformIds.push_back(platformTag(platform, platformId));
    node->firstSeenAt = now;
    node->lastSeenAt = now;
    node->weight = 1.0;
    node->familiarity = Familiarity::Stranger;
    const std::string id = node->internalId;
    nodes_.push_back(std::move(node));

    // 首次接触建立 stranger 状态；不再写 "intimacy = 0.1" 的刷分起点
    ensureEdge(mioId_, id);

    result.firstEncounter = true;
    result.internalId = id;
    if (!saveLocked())
        log::error("RelationshipGraph", "首次遇见落盘失败（内存已更新）");
    return result;
}

void RelationshipGraph::ensureMio(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (findNode(mioId_) != nullptr) return;
    auto node = std::make_unique<PersonNode>();
    node->internalId = mioId_;
    node->name = name;
    nodes_.push_back(std::move(node));
    if (!saveLocked())
        log::error("RelationshipGraph", "ensureMio 落盘失败（内存已更新）");
}

bool RelationshipGraph::setNickname(const std::string& currentName,
                                    const std::string& newName) {
    std::lock_guard<std::mutex> lock(mtx_);
    // 歧义名不绑定（重名时宁可失败，也不改错人）
    const auto matches = findByNameAllLocked(currentName);
    if (matches.size() != 1) return false;
    if (matches.front()->internalId == mioId_) return false;
    // 目标名不得与他人重名（避免制造歧义）
    if (!newName.empty() && !findByNameAllLocked(newName).empty()) return false;
    PersonNode* n = findNode(matches.front()->internalId);
    if (n == nullptr) return false;
    n->name = newName;
    if (!saveLocked())
        log::error("RelationshipGraph", "setNickname 落盘失败");
    return true;
}

bool RelationshipGraph::setNotes(const std::string& name,
                                 const std::string& notes) {
    std::lock_guard<std::mutex> lock(mtx_);
    const auto matches = findByNameAllLocked(name);
    if (matches.size() != 1) return false;
    if (matches.front()->internalId == mioId_) return false;
    PersonNode* n = findNode(matches.front()->internalId);
    if (n == nullptr) return false;
    n->personal = notes;
    if (!saveLocked())
        log::error("RelationshipGraph", "setNotes 落盘失败");
    return true;
}

bool RelationshipGraph::setPersonal(const std::string& name,
                                    const std::string& personal) {
    return setNotes(name, personal);
}

bool RelationshipGraph::setLoves(const std::string& name,
                                 const std::string& loves) {
    std::lock_guard<std::mutex> lock(mtx_);
    const auto matches = findByNameAllLocked(name);
    if (matches.size() != 1) return false;
    if (matches.front()->internalId == mioId_) return false;
    PersonNode* n = findNode(matches.front()->internalId);
    if (n == nullptr) return false;
    n->loves = loves;
    if (!saveLocked())
        log::error("RelationshipGraph", "setLoves 落盘失败");
    return true;
}

const PersonNode* RelationshipGraph::findByName(
    const std::string& name) const {
    std::lock_guard<std::mutex> lock(mtx_);
    // 重名 → 歧义 → nullptr（不得合并身份，也不得任意绑定其中一个）
    const auto matches = findByNameAllLocked(name);
    return matches.size() == 1 ? matches.front() : nullptr;
}

std::vector<const PersonNode*> RelationshipGraph::findByNameAll(
    const std::string& name) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return findByNameAllLocked(name);
}

bool RelationshipGraph::findByNameUnique(const std::string& name,
                                         std::string* internalId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const auto matches = findByNameAllLocked(name);
    if (matches.size() != 1) return false;
    if (internalId) *internalId = matches.front()->internalId;
    return true;
}

const PersonNode* RelationshipGraph::findByPlatform(
    const std::string& platform, const std::string& platformId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return findByPlatformLocked(platform, platformId);
}

const PersonNode* RelationshipGraph::findById(
    const std::string& internalId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return findNode(internalId);
}

std::string RelationshipGraph::nameOf(const std::string& internalId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const PersonNode* n = findNode(internalId);
    return n ? n->name : internalId;
}

std::vector<const PersonNode*> RelationshipGraph::topK(
    std::size_t k) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<const PersonNode*> people;
    for (const auto& n : nodes_)
        if (n->internalId != mioId_) people.push_back(n.get());
    // 权重降序；同权重按 internalId 升序 → 两次重建/重启后顺序一致
    std::sort(people.begin(), people.end(),
              [](const PersonNode* a, const PersonNode* b) {
                  if (a->weight != b->weight) return a->weight > b->weight;
                  return a->internalId < b->internalId;
              });
    if (people.size() > k) people.resize(k);
    return people;
}

std::vector<const PersonNode*> RelationshipGraph::allPersons() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<const PersonNode*> out;
    out.reserve(nodes_.size());
    for (const auto& n : nodes_) {
        if (n->internalId != mioId_) {
            out.push_back(n.get());
        }
    }
    return out;
}

std::string RelationshipGraph::extractQq(const PersonNode& node) {
    for (const auto& pid : node.platformIds) {
        if (pid.rfind("[qq][", 0) == 0 && pid.back() == ']') {
            return pid.substr(5, pid.size() - 6);
        }
        if (pid.rfind("[napcat][", 0) == 0 && pid.back() == ']') {
            return pid.substr(9, pid.size() - 10);
        }
        if (pid.rfind("[onebot][", 0) == 0 && pid.back() == ']') {
            return pid.substr(9, pid.size() - 10);
        }
    }
    for (const auto& pid : node.platformIds) {
        if (!pid.empty() && pid.front() != '[' &&
            std::all_of(pid.begin(), pid.end(), [](unsigned char c) { return std::isdigit(c); })) {
            return pid;
        }
    }
    return "";
}

double RelationshipGraph::intimacy(const std::string& a,
                                   const std::string& b) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const RelationshipEdge* e = findEdge(a, b);
    return e ? e->intimacy : 0.0;
}

void RelationshipGraph::setTrust(const std::string& a, const std::string& b,
                                 double value) {
    std::lock_guard<std::mutex> lock(mtx_);
    RelationshipEdge* e = ensureEdge(a, b);
    if (e->a == a) e->trustAB = value;
    else e->trustBA = value;
    ++e->version;
    if (!saveLocked())
        log::error("RelationshipGraph", "setTrust 落盘失败");
}

double RelationshipGraph::trust(const std::string& a,
                                const std::string& b) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const RelationshipEdge* e = findEdge(a, b);
    if (!e) return 0.5;
    return e->a == a ? e->trustAB : e->trustBA;
}

std::uint64_t RelationshipGraph::interactionCount(const std::string& a,
                                                  const std::string& b) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const RelationshipEdge* e = findEdge(a, b);
    return e ? e->interactionCount : 0;
}

bool RelationshipGraph::isValidRelationshipType(const std::string& type) {
    // 空 = 清除标签；非空只接受小写字母/下划线，避免把任意文本渲染进 prompt
    return type.empty() || regexLikeSimpleType(type);
}

bool RelationshipGraph::isTrustedRelationshipSource(const std::string& source) {
    // family/partner 是亲属/角色关系：不允许自动规则凭互动数据推断出来，
    // 只接受系统初始化、用户明确确认或未来人工审核。
    static const std::set<std::string> trusted = {
        "system_init", "user_confirmed", "human_review", "human_review_service",
        "admin"};
    return trusted.count(source) > 0;
}

bool RelationshipGraph::setRelationshipType(const std::string& personId,
                                            const std::string& relationshipType,
                                            const std::string& source,
                                            std::int64_t now,
                                            std::string* error) {
    if (!isValidRelationshipType(relationshipType)) {
        if (error) *error = "非法关系标签（只允许小写字母/下划线，≤32 字符）";
        return false;
    }
    if (source.empty()) {
        if (error) *error = "关系类别变更必须带来源";
        return false;
    }
    if (source.rfind("model", 0) == 0) {
        // 模型只能提交 pending 关系提案；本入口是自动规则 / 人工审核专用
        if (error) *error = "模型来源不得直接修改关系类别（只能提交提案）";
        return false;
    }
    if ((relationshipType == "family" || relationshipType == "partner") &&
        !isTrustedRelationshipSource(source)) {
        if (error)
            *error = "family/partner 只能由系统初始化、用户明确确认或人工审核设置";
        return false;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    PersonNode* n = findNode(personId);
    if (n == nullptr) {
        if (error) *error = "人物不存在（身份必须由服务端解析为内部 ID）";
        return false;
    }
    n->relationshipType = relationshipType;
    n->relationshipUpdatedAt = now > 0 ? now : (clock_ ? clock_() : 0);
    if (!saveLocked()) {
        if (error) *error = "关系标签落盘失败";
        return false;
    }
    if (error) error->clear();
    return true;
}

bool RelationshipGraph::setBlocked(const std::string& personId, bool blocked,
                                   std::int64_t now) {
    std::lock_guard<std::mutex> lock(mtx_);
    PersonNode* n = findNode(personId);
    if (n == nullptr) return false;
    // blocked 是独立交互状态：不改变 relationship_type / familiarity / 分数
    n->blocked = blocked;
    n->relationshipUpdatedAt = now > 0 ? now : (clock_ ? clock_() : 0);
    if (!saveLocked())
        log::error("RelationshipGraph", "setBlocked 落盘失败");
    return true;
}

Familiarity RelationshipGraph::familiarityOf(
    const std::string& personId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const PersonNode* n = findNode(personId);
    return n ? n->familiarity : Familiarity::Stranger;
}

RelationshipState RelationshipGraph::relationshipState(
    const std::string& personId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    RelationshipState st;
    st.personId = personId;
    const PersonNode* n = findNode(personId);
    if (n == nullptr) return st;  // 未知身份 = 首次接触前的 stranger
    st.relationshipType = n->relationshipType;
    st.familiarity = n->familiarity;
    st.blocked = n->blocked;
    st.legacyUnverified = n->legacyUnverified;
    st.source = n->legacyUnverified ? "legacy_graph" : "relationship_graph";
    st.updatedAt = n->relationshipUpdatedAt > 0 ? n->relationshipUpdatedAt
                                                : n->lastSeenAt;
    if (const RelationshipEdge* e = findEdge(mioId_, personId); e != nullptr) {
        st.intimacyScore = e->intimacy;  // 只作为分数外显，不渲染成等级
        st.trustScore = (e->a == mioId_) ? e->trustAB : e->trustBA;
        st.confidence = n->legacyUnverified ? 0.0 : 1.0;
    }
    return st;
}

std::string RelationshipGraph::mioId() const { return mioId_; }

void RelationshipGraph::load() {
    std::lock_guard<std::mutex> lock(mtx_);
    loadLocked();
}

bool RelationshipGraph::reload() {
    std::lock_guard<std::mutex> lock(mtx_);
    loadLocked();
    return !degraded_;
}

bool RelationshipGraph::storageDegraded() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return degraded_;
}

std::string RelationshipGraph::lastLoadError() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return lastLoadError_;
}

void RelationshipGraph::loadLocked() {
    nodes_.clear();
    edges_.clear();
    rootExtra_ = nlohmann::json::object();
    nodeExtra_.clear();
    edgeExtra_.clear();
    degraded_ = false;
    lastLoadError_.clear();
    nextInternalId_ = 1;
    mioId_ = "mio";

    std::string text;
    std::string readErr;
    const bool exists = jsonstore::readFile(file_, &text, &readErr);
    if (!exists) {
        if (!readErr.empty()) {
            degraded_ = true;
            lastLoadError_ = readErr;
            log::error("RelationshipGraph",
                       "图谱文件读取失败（保留原文件，以空图继续）: " + readErr);
            return;
        }
        // 首次运行：尝试旧版 people.json 迁移
        const auto legacy = file_.parent_path() / "people.json";
        std::string legacyText;
        if (jsonstore::readFile(legacy, &legacyText, nullptr)) {
            try {
                const nlohmann::json j = nlohmann::json::parse(legacyText);
                nextInternalId_ = j.value("nextId", std::uint64_t{1});
                if (auto it = j.find("people"); it != j.end() && it->is_array()) {
                    for (const auto& item : *it) {
                        auto n = std::make_unique<PersonNode>();
                        n->internalId = item.value("internalId", "");
                        n->name = item.value("nickname", "");
                        n->personal = item.value("notes", "");
                        if (auto p = item.find("platformIds");
                            p != item.end() && p->is_array())
                            for (const auto& pid : *p)
                                n->platformIds.push_back(pid);
                        n->weight = item.value("weight", 0.0);
                        n->firstSeenAt =
                            item.value("firstSeenAt", std::int64_t{0});
                        n->lastSeenAt =
                            item.value("lastSeenAt", std::int64_t{0});
                        // 旧数据不可信：personal/loves/attrs 未验证；
                        // 旧 weight 只能保守推断 acquaintance，绝不推断 family/friend
                        n->legacyUnverified = true;
                        n->familiarity =
                            n->weight >= kLegacyAcquaintanceWeight
                                ? Familiarity::Acquaintance
                                : Familiarity::Stranger;
                        const double w = n->weight;
                        const std::string id = n->internalId;
                        nodes_.push_back(std::move(n));
                        // 旧权重 → intimacy 分数（仅活跃统计，不证明关系类别）
                        ensureEdge(mioId_, id)->intimacy =
                            std::min(1.0, w / 10.0);
                    }
                }
                if (saveLocked())
                    log::info("RelationshipGraph",
                              "已从 people.json 迁移到 " + file_.string());
            } catch (const std::exception& e) {
                log::warn("RelationshipGraph",
                          std::string("people.json 迁移失败: ") + e.what());
                nodes_.clear();
                edges_.clear();
            }
        }
        return;
    }

    nlohmann::json j;
    std::string err;
    if (!jsonstore::parse(text, &j, &err)) {
        // 硬约束：迁移失败不得破坏原文件 → 保留原文件、以空图继续、拒绝后续覆盖
        degraded_ = true;
        lastLoadError_ = err;
        nodes_.clear();
        edges_.clear();
        log::error("RelationshipGraph",
                   "图谱文件解析失败（原文件保留未动，以空图继续，可 reload 重试）: " +
                       err);
        return;
    }
    if (!j.is_object()) {
        degraded_ = true;
        lastLoadError_ = "图谱根节点不是 JSON 对象";
        log::error("RelationshipGraph", lastLoadError_ + "（原文件保留未动）");
        return;
    }
    const bool hasSchema = j.contains("schema");
    if (hasSchema && j.value("schema", std::string()) != std::string(kSchema)) {
        degraded_ = true;
        lastLoadError_ = "schema 不匹配: " + j.value("schema", std::string());
        log::error("RelationshipGraph",
                   lastLoadError_ + "（拒绝覆盖，保留原文件）");
        return;
    }
    if (hasSchema && j.value("version", 0) > kSchemaVersion) {
        degraded_ = true;
        lastLoadError_ =
            "图谱版本高于本程序支持: " + std::to_string(j.value("version", 0));
        log::error("RelationshipGraph",
                   lastLoadError_ + "（拒绝覆盖，保留原文件）");
        return;
    }
    // 旧格式（无 schema）：迁移前先备份，再改写为新格式
    const bool oldFormat = !hasSchema;
    if (oldFormat) {
        std::string bakErr;
        const std::int64_t now = clock_ ? clock_() : 0;
        if (!jsonstore::backupIfAbsent(file_, now, &bakErr)) {
            degraded_ = true;
            lastLoadError_ = "迁移备份失败: " + bakErr;
            log::error("RelationshipGraph",
                       lastLoadError_ + "（不迁移，保留原文件）");
            return;
        }
    }

    try {
        nextInternalId_ = j.value("nextId", std::uint64_t{1});
        mioId_ = j.value("mioId", std::string("mio"));
        if (auto it = j.find("nodes"); it != j.end() && it->is_array()) {
            for (const auto& item : *it) {
                if (!item.is_object()) continue;
                auto n = std::make_unique<PersonNode>();
                n->internalId = item.value("internalId", "");
                n->name = item.value("name", "");
                n->personal = item.value("personal", "");
                n->loves = item.value("loves", "");
                if (auto p = item.find("platformIds");
                    p != item.end() && p->is_array())
                    for (const auto& pid : *p)
                        if (pid.is_string())
                            n->platformIds.push_back(pid.get<std::string>());
                n->weight = item.value("weight", 0.0);
                n->firstSeenAt = item.value("firstSeenAt", std::int64_t{0});
                n->lastSeenAt = item.value("lastSeenAt", std::int64_t{0});
                n->blocked = item.value("blocked", false);
                n->interactionEvents =
                    item.value("interaction_events", std::uint64_t{0});
                n->relationshipUpdatedAt =
                    item.value("relationship_updated_at", std::int64_t{0});
                n->relationshipType =
                    item.value("relationship_type", std::string());
                nlohmann::json attrsExtra = nlohmann::json::object();
                if (auto a = item.find("attrs");
                    a != item.end() && a->is_object()) {
                    for (auto ita = a->begin(); ita != a->end(); ++ita) {
                        if (ita->is_string()) {
                            n->attrs[ita.key()] = ita->get<std::string>();
                        } else {
                            // 非字符串属性：保留原值（写回不丢字段）
                            attrsExtra[ita.key()] = *ita;
                        }
                    }
                }
                // 熟悉度：新格式读显式字段；旧格式按旧 weight 保守推断
                Familiarity fam = Familiarity::Stranger;
                bool famSet = false;
                if (auto f = item.find("familiarity");
                    f != item.end() && f->is_string()) {
                    famSet = parseFamiliarity(f->get<std::string>(), fam);
                    if (!famSet) fam = Familiarity::Stranger;  // 未知值保持最窄
                }
                // legacy 标记：新格式读显式字段；旧格式整份文件都不可信
                const bool legacy =
                    item.value("legacy_unverified", oldFormat) || oldFormat;
                n->legacyUnverified = legacy;
                if (famSet) {
                    n->familiarity = fam;
                } else if (legacy && n->weight >= kLegacyAcquaintanceWeight) {
                    n->familiarity = Familiarity::Acquaintance;  // 绝不到 family
                } else {
                    n->familiarity = Familiarity::Stranger;
                }
                if (!attrsExtra.empty()) {
                    putExtra(nodeExtra_, n->internalId, "attrs_nonstring",
                             attrsExtra);
                }
                // 保留不认识的旧字段
                for (auto ita = item.begin(); ita != item.end(); ++ita) {
                    if (managedNodeKeys().count(ita.key()) > 0) continue;
                    putExtra(nodeExtra_, n->internalId, ita.key(), ita.value());
                }
                nextInternalId_ =
                    std::max(nextInternalId_, j.value("nextId", std::uint64_t{1}));
                nodes_.push_back(std::move(n));
            }
        }
        if (auto it = j.find("edges"); it != j.end() && it->is_array()) {
            for (const auto& item : *it) {
                if (!item.is_object()) continue;
                auto e = std::make_unique<RelationshipEdge>();
                e->a = item.value("a", "");
                e->b = item.value("b", "");
                e->intimacy = item.value("intimacy", 0.0);
                e->trustAB = item.value("trustAB", 0.5);
                e->trustBA = item.value("trustBA", 0.5);
                e->lastInteractionAt =
                    item.value("last_interaction_at", std::int64_t{0});
                e->interactionCount =
                    item.value("interaction_count", std::uint64_t{0});
                e->version = item.value("version", std::uint64_t{0});
                if (auto k = item.find("event_keys");
                    k != item.end() && k->is_array())
                    for (const auto& s : *k)
                        if (s.is_string() &&
                            e->eventKeys.size() < kMaxEventKeys)
                            e->eventKeys.push_back(s.get<std::string>());
                const std::string ekey = edgeKey(e->a, e->b);
                for (auto ita = item.begin(); ita != item.end(); ++ita) {
                    if (managedEdgeKeys().count(ita.key()) > 0) continue;
                    putExtra(edgeExtra_, ekey, ita.key(), ita.value());
                }
                edges_.push_back(std::move(e));
            }
        }
        // 未知根字段原样保留
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == "schema" || it.key() == "version" ||
                it.key() == "nextId" || it.key() == "mioId" ||
                it.key() == "nodes" || it.key() == "edges")
                continue;
            rootExtra_[it.key()] = it.value();
        }
    } catch (const std::exception& e) {
        // 结构异常：同样视为迁移失败 → 保留原文件、空图、拒绝覆盖
        degraded_ = true;
        lastLoadError_ = std::string("图谱结构解析异常: ") + e.what();
        nodes_.clear();
        edges_.clear();
        log::error("RelationshipGraph",
                   lastLoadError_ + "（原文件保留未动，可 reload 重试）");
        return;
    }

    if (oldFormat) {
        log::info("RelationshipGraph",
                  "检测到旧版图谱格式：已备份并迁移（legacy_unverified 标记）");
        saveLocked();
    }
}

void RelationshipGraph::save() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!saveLocked()) {
        if (degraded_) {
            log::error("RelationshipGraph",
                       "图谱处于降级状态，拒绝覆盖原文件: " + lastLoadError_);
        } else {
            log::error("RelationshipGraph", "图谱落盘失败");
        }
    }
}

bool RelationshipGraph::saveLocked() {
    if (degraded_) {
        // 硬约束：迁移/加载失败后绝不覆盖原文件（重试请先 reload）
        return false;
    }
    nlohmann::json j =
        rootExtra_.is_object() ? rootExtra_ : nlohmann::json::object();
    j["schema"] = kSchema;
    j["version"] = kSchemaVersion;
    j["nextId"] = nextInternalId_;
    j["mioId"] = mioId_;
    auto nodes = nlohmann::json::array();
    for (const auto& n : nodes_) {
        nlohmann::json item;
        item["internalId"] = n->internalId;
        item["name"] = n->name;
        item["personal"] = n->personal;
        item["loves"] = n->loves;
        auto pids = nlohmann::json::array();
        for (const auto& pid : n->platformIds) pids.push_back(pid);
        item["platformIds"] = pids;
        item["weight"] = n->weight;
        item["firstSeenAt"] = n->firstSeenAt;
        item["lastSeenAt"] = n->lastSeenAt;
        item["relationship_type"] = n->relationshipType;
        item["familiarity"] = toString(n->familiarity);
        item["blocked"] = n->blocked;
        item["legacy_unverified"] = n->legacyUnverified;
        item["interaction_events"] = n->interactionEvents;
        item["relationship_updated_at"] = n->relationshipUpdatedAt;
        if (!n->attrs.empty()) item["attrs"] = n->attrs;
        // 向前兼容：合并不认识的旧字段（含非字符串 attrs）
        if (auto it = nodeExtra_.find(n->internalId); it != nodeExtra_.end()) {
            for (auto x = it->second.begin(); x != it->second.end(); ++x) {
                if (x.key() == "attrs_nonstring" && x->is_object()) {
                    if (!item.contains("attrs") || !item["attrs"].is_object())
                        item["attrs"] = nlohmann::json::object();
                    for (auto y = x->begin(); y != x->end(); ++y)
                        item["attrs"][y.key()] = y.value();
                    continue;
                }
                item[x.key()] = x.value();
            }
        }
        nodes.push_back(std::move(item));
    }
    auto edges = nlohmann::json::array();
    for (const auto& e : edges_) {
        nlohmann::json item = {{"a", e->a},
                               {"b", e->b},
                               {"intimacy", e->intimacy},
                               {"trustAB", e->trustAB},
                               {"trustBA", e->trustBA},
                               {"last_interaction_at", e->lastInteractionAt},
                               {"interaction_count", e->interactionCount},
                               {"version", e->version},
                               {"event_keys", e->eventKeys}};
        if (auto it = edgeExtra_.find(edgeKey(e->a, e->b));
            it != edgeExtra_.end()) {
            for (auto x = it->second.begin(); x != it->second.end(); ++x)
                item[x.key()] = x.value();
        }
        edges.push_back(std::move(item));
    }
    j["nodes"] = std::move(nodes);
    j["edges"] = std::move(edges);

    std::string err;
    if (!jsonstore::writeAtomic(file_, j.dump() + "\n", &err)) {
        lastLoadError_ = err;
        log::error("RelationshipGraph", "原子写失败: " + err);
        return false;
    }
    return true;
}

} // namespace mio
