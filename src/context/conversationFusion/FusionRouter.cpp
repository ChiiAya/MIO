// FusionRouter 实现（见 context/conversationFusion/FusionRouter.h）
//
// 锁策略：route/setPublic 全程持 mapMtx_（映射串行化）；unit 内容只在
// Runtime 拿到 unit 后持 unit->mtx 修改 —— route 期间不锁 unit，避免嵌套。
// fusion 是尽力而为的派生状态：启动后按消息流增量路由/融合，不落盘。

#include "context/conversationFusion/FusionRouter.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "context/costEstimator/Tokens.h"
#include "log/Log.h"
#include "core/math/VectorMath.h"

namespace mio {

namespace {

constexpr int kTopicEmbedCooldownSeconds = 60;  // 向量化失败冷却
constexpr std::size_t kTopicVecCacheCap = 128;  // 话题向量缓存上限

std::int64_t unitTokens(const FusionUnit& u) {
    return estimateTokens(u.context());
}

std::string hexEncode(const std::string& s) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 0x0F];
    }
    return out;
}

} // namespace

FusionRouter::FusionRouter(RelationshipGraph& graph, Achieve& achieve,
                           std::shared_ptr<SummaryManager> summary, FusionConfig cfg,
                           std::shared_ptr<Embedding> embedding)
    : graph_(graph), achieve_(achieve), summary_(std::move(summary)), cfg_(cfg),
      embedding_(std::move(embedding)) {}

void FusionRouter::update(FusionConfig cfg, std::shared_ptr<SummaryManager> summary,
                          std::shared_ptr<Embedding> embedding) {
    std::lock_guard<std::mutex> lock(mapMtx_);
    cfg_ = cfg;
    summary_ = std::move(summary);
    embedding_ = std::move(embedding);
    topicVecCache_.clear();
}

void FusionRouter::updateConfig(FusionConfig cfg) {
    std::lock_guard<std::mutex> lock(mapMtx_);
    cfg_ = cfg;
}

void FusionRouter::updateDependencies(std::shared_ptr<SummaryManager> summary,
                                      std::shared_ptr<Embedding> embedding) {
    std::lock_guard<std::mutex> lock(mapMtx_);
    summary_ = std::move(summary);
    embedding_ = std::move(embedding);
    topicVecCache_.clear();
}

std::size_t FusionRouter::unitCount() const {
    std::lock_guard<std::mutex> lock(mapMtx_);
    return units_.size();
}

std::vector<FusionRouter::UnitSnapshot> FusionRouter::activeUnitsSince(
    std::int64_t since) const {
    // 两阶段快照：第一阶段只在 mapMtx_ 下取指针与来源 key（不碰 unit 锁），
    // 第二阶段逐个取 unit 锁拷贝上下文。反过来同时持 mapMtx_ + unit->mtx 会和
    // ingest 的 unit->mtx → mapMtx_（set_public 等工具回调 Router）形成反向锁序。
    // units_ 从不 erase（融合只产生孤儿 unit），所以第二阶段的裸指针仍然有效。
    struct Ref {
        FusionUnit* unit = nullptr;
        ConversationKey conversation;
    };
    std::vector<Ref> refs;
    {
        std::lock_guard<std::mutex> lock(mapMtx_);
        refs.reserve(units_.size());
        for (const auto& entry : units_) {
            const auto& unit = entry.second;
            if (!unit || unit->inDegree().empty()) continue;
            Ref ref;
            ref.unit = unit.get();
            ref.conversation = unit->inDegree().front();
            refs.push_back(std::move(ref));
        }
    }

    std::vector<UnitSnapshot> out;
    for (auto& ref : refs) {
        std::lock_guard<std::mutex> unitLock(ref.unit->mtx);
        UnitSnapshot snapshot;
        snapshot.conversation = ref.conversation;
        snapshot.context = ref.unit->context();
        snapshot.topic = ref.unit->topic();
        if (!snapshot.context.empty())
            snapshot.lastAt = snapshot.context.back().createdAt;
        if (snapshot.lastAt >= since) out.push_back(std::move(snapshot));
    }
    return out;
}

FusionUnit* FusionRouter::findUnit(const IncomingMessage& msg) const {
    const std::string sourceKey = msg.conversation.toString();
    if (auto it = sourceToUnit_.find(sourceKey); it != sourceToUnit_.end()) {
        auto u = units_.find(it->second);
        if (u != units_.end()) return u->second.get();
    }
    // 私聊兜底：按人路由（同人不同平台 → 同一 unit）
    if (msg.conversation.scope == ConversationScope::Private) {
        if (const PersonNode* p = graph_.findByPlatform(msg.platform, msg.senderId);
            p != nullptr) {
            if (auto it = personToUnit_.find(p->internalId);
                it != personToUnit_.end()) {
                auto u = units_.find(it->second);
                if (u != units_.end()) return u->second.get();
            }
        }
    }
    return nullptr;
}

FusionUnit* FusionRouter::createFromColdRead(const IncomingMessage& msg,
                                             std::time_t now) {
    // 首次遇见来源：Achieve 冷读取（摘要前20条 + 原文5条）→ 创建 FusionUnit
    ColdRead cold;
    if (summary_) {
        cold = achieve_.coldRead(msg.conversation, *summary_);
    } else {
        cold.msgs = achieve_.load(msg.conversation);
    }
    auto unit = std::make_unique<FusionUnit>(msg.conversation, cold.msgs);
    unit->setIsPublic(cold.isPublic);
    if (!cold.topic.empty()) unit->setTopic(cold.topic);  // 话题随摘要同步更新

    const std::string unitKey = msg.conversation.toString();
    FusionUnit* raw = unit.get();
    units_[unitKey] = std::move(unit);
    sourceToUnit_[unitKey] = unitKey;
    if (msg.conversation.scope == ConversationScope::Private) {
        if (const PersonNode* p = graph_.findByPlatform(msg.platform, msg.senderId);
            p != nullptr) {
            unitIsPerson_[unitKey] = true;
            unitPersonId_[unitKey] = p->internalId;
            personToUnit_[p->internalId] = unitKey;
        }
    } else {
        unitIsPerson_[unitKey] = false;
        unitMembers_[unitKey] = {};
    }
    log::info("FusionRouter",
              "冷读取创建 unit: " + unitKey +
                  " topic=" + raw->topic() +
                  " isPublic=" + (raw->isPublic() ? "true" : "false") +
                  " topic_hex=" + hexEncode(raw->topic()));
    return raw;
}

double FusionRouter::intimacyBetween(FusionUnit& a, FusionUnit& b) const {
    const std::string ka = a.inDegree().empty() ? "" : a.inDegree().front().toString();
    const std::string kb = b.inDegree().empty() ? "" : b.inDegree().front().toString();
    const bool aPerson = !ka.empty() && unitIsPerson_.count(ka) && unitIsPerson_.at(ka);
    const bool bPerson = !kb.empty() && unitIsPerson_.count(kb) && unitIsPerson_.at(kb);

    // 私聊↔群聊：私聊人物在群里 → 默认 100%
    if (aPerson && !bPerson) {
        auto it = unitMembers_.find(kb);
        if (it == unitMembers_.end()) return 0.0;
        return it->second.count(unitPersonId_.at(ka)) > 0 ? 1.0 : 0.0;
    }
    if (bPerson && !aPerson) {
        auto it = unitMembers_.find(ka);
        if (it == unitMembers_.end()) return 0.0;
        return it->second.count(unitPersonId_.at(kb)) > 0 ? 1.0 : 0.0;
    }
    // 私聊↔私聊：图谱边 intimacy
    if (aPerson && bPerson) {
        return graph_.intimacy(unitPersonId_.at(ka), unitPersonId_.at(kb));
    }
    // 群↔群：成员对边 intimacy 最大值
    const auto& ma = unitMembers_.at(ka);
    const auto& mb = unitMembers_.at(kb);
    double best = 0.0;
    for (const auto& x : ma)
        for (const auto& y : mb) best = std::max(best, graph_.intimacy(x, y));
    return best;
}

double FusionRouter::trustBetween(FusionUnit& a, FusionUnit& b) const {
    const std::string ka = a.inDegree().front().toString();
    const std::string kb = b.inDegree().front().toString();
    const bool aPerson = unitIsPerson_.count(ka) && unitIsPerson_.at(ka);
    const bool bPerson = unitIsPerson_.count(kb) && unitIsPerson_.at(kb);
    if (aPerson && bPerson) {
        const std::string& x = unitPersonId_.at(ka);
        const std::string& y = unitPersonId_.at(kb);
        return std::max(graph_.trust(x, y), graph_.trust(y, x));
    }
    // 涉及群聊：v1 用 MIO 与该人的信任（默认 0.5 ≥ 阈值 0.4 可通过）；
    // trust 与隐私政策的深度消费留待后续
    if (aPerson || bPerson) {
        const std::string& pid = aPerson ? unitPersonId_.at(ka)
                                         : unitPersonId_.at(kb);
        return std::max(graph_.trust(pid, graph_.mioId()),
                        graph_.trust(graph_.mioId(), pid));
    }
    // 群↔群：成员对 trust 最大值
    const auto& ma = unitMembers_.at(ka);
    const auto& mb = unitMembers_.at(kb);
    double best = 0.0;
    for (const auto& x : ma)
        for (const auto& y : mb)
            best = std::max(best, std::max(graph_.trust(x, y),
                                           graph_.trust(y, x)));
    return best;
}

bool FusionRouter::canFuse(FusionUnit& a, FusionUnit& b) const {
    const std::string aKey = a.inDegree().empty() ? "?" : a.inDegree().front().toString();
    const std::string bKey = b.inDegree().empty() ? "?" : b.inDegree().front().toString();
    const std::string common =
        "canFuse: a=" + aKey + " b=" + bKey +
        " topicA=" + a.topic() + " topicB=" + b.topic() +
        " topicA_hex=" + hexEncode(a.topic()) +
        " topicB_hex=" + hexEncode(b.topic()) +
        " isPublicA=" + (a.isPublic() ? "true" : "false") +
        " isPublicB=" + (b.isPublic() ? "true" : "false");

    if (&a == &b) {
        log::info("FusionRouter", common + " => false (同一 unit)");
        return false;
    }
    if (a.inDegree().empty() || b.inDegree().empty()) {
        log::info("FusionRouter", common + " => false (存在孤儿 unit)");
        return false;
    }
    if (!a.isPublic() || !b.isPublic()) {
        log::info("FusionRouter", common + " => false (私密闸门)");
        return false;
    }

    const double intimacy = intimacyBetween(a, b);
    if (intimacy < cfg_.intimacyThreshold) {
        log::info("FusionRouter", common +
                                      " intimacy=" + std::to_string(intimacy) +
                                      " threshold=" +
                                      std::to_string(cfg_.intimacyThreshold) +
                                      " => false (熟悉度不足)");
        return false;
    }

    const double trust = trustBetween(a, b);
    if (trust < cfg_.trustThreshold) {
        log::info("FusionRouter", common +
                                      " intimacy=" + std::to_string(intimacy) +
                                      " trust=" + std::to_string(trust) +
                                      " threshold=" +
                                      std::to_string(cfg_.trustThreshold) +
                                      " => false (信任度不足)");
        return false;
    }

    const bool topicOk = topicsRelated(a.topic(), b.topic());
    if (!topicOk) {
        log::info("FusionRouter", common +
                                      " intimacy=" + std::to_string(intimacy) +
                                      " trust=" + std::to_string(trust) +
                                      " => false (话题不相关)");
        return false;
    }

    log::info("FusionRouter", common +
                                  " intimacy=" + std::to_string(intimacy) +
                                  " trust=" + std::to_string(trust) +
                                  " topicRelated=true => true");
    return true;
}

std::vector<float> FusionRouter::topicVector(const std::string& topic) const {
    if (auto it = topicVecCache_.find(topic); it != topicVecCache_.end()){
        log::info("FusionRouter","Embedding Cache Hit");
        return it->second;
    }

    if (embedding_ == nullptr || !cfg_.topicSemanticMatch) return {};
    if (std::time(nullptr) < embedCooldownUntil_) return {};

    try {
        log::info("FusionRouter","Embedding Cache Failed, Generating");
        std::vector<float> vec = embedding_->embed(topic);
        if (vecmath::normalizeInPlace(vec)) {
            if (topicVecCache_.size() >= kTopicVecCacheCap)
                topicVecCache_.clear();  // 简单防膨胀：话题集合有限，清空后重热
            topicVecCache_.emplace(topic, vec);
            return vec;
        }
        return {};
    } catch (const std::exception& e) {
        embedCooldownUntil_ = std::time(nullptr) + kTopicEmbedCooldownSeconds;
        log::warn("FusionRouter",
                  std::string("话题向量化失败（回退精确匹配，") +
                      std::to_string(kTopicEmbedCooldownSeconds) + "s 冷却）: " +
                      e.what() + " | topic=" + topic +
                      " | topic_hex=" + hexEncode(topic));
        return {};
    }
}

bool FusionRouter::topicsRelated(const std::string& a,
                                 const std::string& b) const {
    // v1 语义保留：任一话题为空视为相关（预热期不设闸）
    if (a.empty() || b.empty()) return true;

    const std::vector<float> va = topicVector(a);
    const std::vector<float> vb = topicVector(b);
    // 任一/topics 全部未命中向量（未配置、失败冷却）→ 仍按 v1 精确匹配降级
    if (va.empty() || vb.empty()) {
        const bool related = (a == b);
        log::info("FusionRouter",
                  "话题向量不可用，回退精确匹配: " + a + " / " + b +
                      " => " + (related ? "true" : "false"));
        return related;
    }

    // 双侧已归一化：点积即余弦
    const double sim = vecmath::dot(va.data(), vb.data(), va.size());
    const bool related = sim >= cfg_.topicSimilarityThreshold;
    log::info("FusionRouter",
              "话题语义相似度: " + a + " / " + b +
                  " sim=" + std::to_string(sim) +
                  " threshold=" + std::to_string(cfg_.topicSimilarityThreshold) +
                  " => " + (related ? "true" : "false"));
    return related;
}

FusionUnit* FusionRouter::fuse(FusionUnit& a, FusionUnit& b,
                               std::time_t now) {
    FusionUnit* short_ = nullptr;
    FusionUnit* long_ = nullptr;
    if (unitTokens(a) <= unitTokens(b)) {
        short_ = &a;
        long_ = &b;
    } else {
        short_ = &b;
        long_ = &a;
    }

    // 短者 beRedirected（摘要前20 + 近5原文）→ 追加到长者末尾
    const std::vector<Msg> redirected = summary_ ? short_->beRedirected(*summary_)
                                                 : short_->beRedirected();
    for (const auto& m : redirected) long_->append(m);

    const std::string shortKey = short_->inDegree().front().toString();
    const std::string longKey = long_->inDegree().front().toString();

    // 入度合并 + 映射重定向（来源→长者）
    const bool shortIsPerson = unitIsPerson_.count(shortKey) && unitIsPerson_.at(shortKey);
    for (const auto& k : short_->inDegree()) {
        long_->addInDegree(k);
        sourceToUnit_[k.toString()] = longKey;
    }
    if (shortIsPerson) personToUnit_[unitPersonId_.at(shortKey)] = longKey;
    // 群成员集合合并
    if (auto it = unitMembers_.find(shortKey); it != unitMembers_.end()) {
        auto& dst = unitMembers_[longKey];
        dst.insert(it->second.begin(), it->second.end());
        unitMembers_.erase(it);
    }
    short_->clearInDegree();  // 孤儿 unit（保留对象，避免悬垂指针）

    log::info("FusionRouter",
              "融合: " + shortKey + " → " + longKey +
                  " short_topic=" + short_->topic() +
                  " long_topic=" + long_->topic() +
                  " short_topic_hex=" + hexEncode(short_->topic()) +
                  " long_topic_hex=" + hexEncode(long_->topic()) +
                  " short_isPublic=" + (short_->isPublic() ? "true" : "false") +
                  " long_isPublic=" + (long_->isPublic() ? "true" : "false"));
    return long_;
}

FusionUnit* FusionRouter::route(const IncomingMessage& msg, std::time_t now) {
    std::lock_guard<std::mutex> lock(mapMtx_);

    // 1) 身份映射：先确保身份存在（首次遇见分配 internalId、权重累积），
    //    若为系统未记录个体，注入 nameHint = senderId(senderName)
    const std::string nameHint = (!msg.senderName.empty() && msg.senderName != msg.senderId)
                                     ? msg.senderId + "(" + msg.senderName + ")"
                                     : msg.senderId;
    graph_.onSeen(msg.platform, msg.senderId, nameHint, now);
    FusionUnit* unit = findUnit(msg);
    if (!unit) unit = createFromColdRead(msg, now);

    // 群消息：登记成员 + 共同出现 → 两人亲密累积
    const PersonNode* senderNode = graph_.findByPlatform(msg.platform, msg.senderId);
    if (msg.conversation.scope == ConversationScope::Group && !unit->inDegree().empty()) {
        const std::string unitKey = unit->inDegree().front().toString();
        if (senderNode != nullptr) {
            auto& members = unitMembers_[unitKey];
            for (const auto& m : members)
                if (m != senderNode->internalId)
                    graph_.bumpIntimacy(senderNode->internalId, m, 0.05, now);
            members.insert(senderNode->internalId);
        }
    }

    // @ 关系映射：结构化 mention 优先，纯文本只接收有独立边界的 QQ 号。
    // 将其映射为认识的人，并在群聊中计入成员并累积与发言者的亲密度
    const auto atIds = mentionedUserIds(msg);
    for (const auto& atId : atIds) {
        if (atId.empty() || atId == "all" || atId == msg.senderId) continue;
        graph_.onSeen(msg.platform, atId, atId, now);
        if (msg.conversation.scope == ConversationScope::Group && !unit->inDegree().empty()) {
            const std::string unitKey = unit->inDegree().front().toString();
            if (const PersonNode* atNode = graph_.findByPlatform(msg.platform, atId);
                atNode != nullptr) {
                auto& members = unitMembers_[unitKey];
                members.insert(atNode->internalId);
                if (senderNode != nullptr && senderNode->internalId != atNode->internalId) {
                    graph_.bumpIntimacy(senderNode->internalId, atNode->internalId, 0.05, now);
                }
            }
        }
    }

    // 2) Fuse 判断：与其他 unit 比较，满足条件 → redirect 融合（一次最多一对）
    for (auto& [key, other] : units_) {
        if (other.get() == unit) continue;
        if (canFuse(*unit, *other)) {
            unit = fuse(*unit, *other, now);
            break;
        }
    }
    unit->bumpTurn();  // 对话轮数 +1（topic 预热窗口用；重定向消息不计）
    const std::string unitKey = unit->inDegree().empty()
                                    ? "?"
                                    : unit->inDegree().front().toString();
    log::info("FusionRouter",
              "route: source=" + msg.conversation.toString() +
                  " unit=" + unitKey +
                  " topic=" + unit->topic() +
                  " isPublic=" + (unit->isPublic() ? "true" : "false") +
                  " topic_hex=" + hexEncode(unit->topic()));
    return unit;
}

std::vector<std::string> FusionRouter::mentionedUserIds(const IncomingMessage& msg) {
    std::vector<std::string> ids;
    const bool qq = msg.platform == "qq" || msg.platform == "napcat" ||
                    msg.platform == "onebot";
    auto append = [&](const std::string& id) {
        if (id.empty() || id == "all") return;
        if (qq && !std::all_of(id.begin(), id.end(), [](unsigned char c) {
                return std::isdigit(c);
            })) return;
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    };
    for (const auto& id : msg.atUserIds) append(id);
    if (!qq) return ids;

    auto identifierChar = [](unsigned char c) {
        return c < 128 && (std::isalnum(c) || c == '_');
    };
    for (std::size_t pos = 0; (pos = msg.text.find('@', pos)) != std::string::npos; ++pos) {
        if (pos > 0) {
            const unsigned char prev = msg.text[pos - 1];
            if (identifierChar(prev) || prev == '.' || prev == '+' ||
                prev == '-' || prev == '%' || prev == '@') continue;
        }
        std::size_t end = pos + 1;
        while (end < msg.text.size() && std::isdigit(static_cast<unsigned char>(msg.text[end]))) ++end;
        if (end == pos + 1) continue;
        if (end < msg.text.size() && identifierChar(static_cast<unsigned char>(msg.text[end]))) continue;
        if (end + 1 < msg.text.size() && msg.text[end] == '.' &&
            identifierChar(static_cast<unsigned char>(msg.text[end + 1]))) continue;
        append(msg.text.substr(pos + 1, end - pos - 1));
    }
    return ids;
}

std::string FusionRouter::mentionContext(const std::vector<std::string>& ids,
                                        const std::string& platform) const {
    std::ostringstream result;
    std::set<std::string> rendered;
    for (const auto& id : ids) {
        if (id.empty() || id == "all" || !rendered.insert(id).second) continue;
        std::optional<PersonNode> person;
        try {
            person = graph_.resolveContact(RelationshipGraph::platformTag(platform, id));
        } catch (const std::invalid_argument&) {
            // An unknown mention remains visible with its original platform ID.
        }
        if (result.tellp() == std::streampos(0)) result << "[提及对象]";
        result << " @" << id;
        if (person) {
            result << "=" << person->name << " (internal_id=" << person->internalId << ")";
        } else {
            result << " (尚无称呼记录)";
        }
        result << ';';
    }
    return result.str();
}

std::string FusionRouter::displayName(const IncomingMessage& msg) const {
    // 统一身份码在图谱中有对应身份 → 用昵称；否则回退原始 senderId
    if (const auto p = graph_.platformContact(msg.platform, msg.senderId);
        p && !p->name.empty()) {
        if (p->name == msg.senderId && !msg.senderName.empty() && msg.senderName != msg.senderId) {
            return msg.senderId + "(" + msg.senderName + ")";
        }
        return p->name;
    }
    if (!msg.senderName.empty() && msg.senderName != msg.senderId) {
        return msg.senderId + "(" + msg.senderName + ")";
    }
    return msg.senderId;
}

FusionUnit* FusionRouter::setPublic(const ConversationKey& source,
                                    bool isPublic) {
    std::lock_guard<std::mutex> lock(mapMtx_);
    const std::string sourceKey = source.toString();
    auto it = sourceToUnit_.find(sourceKey);
    if (it == sourceToUnit_.end()) return nullptr;
    FusionUnit* unit = units_.at(it->second).get();

    unit->setIsPublic(isPublic);
    log::info("FusionRouter",
              "set_public: " + sourceKey +
                  " => " + (isPublic ? "公开" : "私密") +
                  " topic=" + unit->topic() +
                  " isPublic=" + (unit->isPublic() ? "true" : "false"));
    return unit;
}

} // namespace mio
