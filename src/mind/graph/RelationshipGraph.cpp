// ============================================================================
// 关系图谱实现（见 mind/RelationshipGraph.h）
// 文件格式 data/relationships.json：
//   { "nextId": 6, "mioId": "mio",
//     "nodes": [ {internalId,name,personal,loves,platformIds,weight,firstSeenAt,lastSeenAt,attrs} ],
//     "edges": [ {a,b,intimacy,trustAB,trustBA} ] }
// 旧版 data/people.json（无 edges/attrs）首次读取时自动迁移。
// ============================================================================

#include "mind/graph/RelationshipGraph.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>
#include "log/Log.h"

namespace mio {

RelationshipGraph::RelationshipGraph(std::filesystem::path file)
    : file_(std::move(file)) {
    load();
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
        // MIO↔此人 亲密熟悉度：同公式衰减 + 增量（delta 0.1/条）
        RelationshipEdge* e = ensureEdge(mioId_, n->internalId);
        e->intimacy =
            std::min(1.0, decayed(e->intimacy, prevLast, now) + 0.1);
        save();
        return result;
    }

    // 首次遇见：分配内部 ID，建立节点 + MIO↔此人 边
    auto node = std::make_unique<PersonNode>();
    node->internalId = "p" + std::to_string(nextInternalId_++);
    node->name = nameHint.empty() ? platformId : nameHint;
    node->platformIds.push_back(platformTag(platform, platformId));
    node->firstSeenAt = now;
    node->lastSeenAt = now;
    node->weight = 1.0;
    const std::string id = node->internalId;
    nodes_.push_back(std::move(node));

    ensureEdge(mioId_, id)->intimacy = 0.1;

    result.firstEncounter = true;
    result.internalId = id;
    save();
    return result;
}

void RelationshipGraph::ensureMio(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (findNode(mioId_) != nullptr) return;
    auto node = std::make_unique<PersonNode>();
    node->internalId = mioId_;
    node->name = name;
    nodes_.push_back(std::move(node));
    save();
}

bool RelationshipGraph::setNickname(const std::string& currentName,
                                    const std::string& newName) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& n : nodes_)
        if (n->name == currentName) {
            if (n->internalId == mioId_) return false;
            n->name = newName;
            save();
            return true;
        }
    return false;
}

bool RelationshipGraph::setNotes(const std::string& name,
                                 const std::string& notes) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& n : nodes_)
        if (n->name == name) {
            if (n->internalId == mioId_) return false;
            n->personal = notes;
            save();
            return true;
        }
    return false;
}

bool RelationshipGraph::setPersonal(const std::string& name,
                                    const std::string& personal) {
    return setNotes(name, personal);
}

bool RelationshipGraph::setLoves(const std::string& name,
                                 const std::string& loves) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& n : nodes_)
        if (n->name == name) {
            if (n->internalId == mioId_) return false;
            n->loves = loves;
            save();
            return true;
        }
    return false;
}

const PersonNode* RelationshipGraph::findByName(
    const std::string& name) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return findByNameLocked(name);
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
    std::sort(people.begin(), people.end(),
              [](const PersonNode* a, const PersonNode* b) {
                  return a->weight > b->weight;
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

void RelationshipGraph::bumpIntimacy(const std::string& a,
                                     const std::string& b, double delta,
                                     std::int64_t now) {
    if (a == b) return;
    std::lock_guard<std::mutex> lock(mtx_);
    RelationshipEdge* e = ensureEdge(a, b);
    const PersonNode* na = findNode(e->a);
    const PersonNode* nb = findNode(e->b);
    const std::int64_t last =
        std::max(na ? na->lastSeenAt : 0, nb ? nb->lastSeenAt : 0);
    e->intimacy = std::min(1.0, decayed(e->intimacy, last, now) + delta);
    save();
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
    save();
}

double RelationshipGraph::trust(const std::string& a,
                                const std::string& b) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const RelationshipEdge* e = findEdge(a, b);
    if (!e) return 0.5;
    return e->a == a ? e->trustAB : e->trustBA;
}

std::string RelationshipGraph::mioId() const { return mioId_; }

void RelationshipGraph::load() {
    std::ifstream in(file_);
    if (!in) {
        // 首次运行：尝试旧版 people.json 迁移
        const auto legacy = file_.parent_path() / "people.json";
        std::ifstream inLegacy(legacy);
        if (inLegacy) {
            try {
                const nlohmann::json j = nlohmann::json::parse(inLegacy);
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
                        const double w = n->weight;
                        const std::string id = n->internalId;
                        nodes_.push_back(std::move(n));
                        // 旧权重 → MIO↔此人 intimacy（近似）
                        ensureEdge(mioId_, id)->intimacy =
                            std::min(1.0, w / 10.0);
                    }
                }
                save();
                log::info("RelationshipGraph",
                          "已从 people.json 迁移到 " + file_.string());
            } catch (const std::exception& e) {
                log::warn("RelationshipGraph",
                          std::string("people.json 迁移失败: ") + e.what());
            }
        }
        return;
    }

    try {
        const nlohmann::json j = nlohmann::json::parse(in);
        nextInternalId_ = j.value("nextId", std::uint64_t{1});
        mioId_ = j.value("mioId", std::string("mio"));
        if (auto it = j.find("nodes"); it != j.end() && it->is_array()) {
            for (const auto& item : *it) {
                auto n = std::make_unique<PersonNode>();
                n->internalId = item.value("internalId", "");
                n->name = item.value("name", "");
                n->personal = item.value("personal", "");
                n->loves = item.value("loves", "");
                if (auto p = item.find("platformIds");
                    p != item.end() && p->is_array())
                    for (const auto& pid : *p) n->platformIds.push_back(pid);
                n->weight = item.value("weight", 0.0);
                n->firstSeenAt = item.value("firstSeenAt", std::int64_t{0});
                n->lastSeenAt = item.value("lastSeenAt", std::int64_t{0});
                if (auto a = item.find("attrs");
                    a != item.end() && a->is_object())
                    for (auto ita = a->begin(); ita != a->end(); ++ita)
                        n->attrs[ita.key()] = ita->get<std::string>();
                nodes_.push_back(std::move(n));
            }
        }
        if (auto it = j.find("edges"); it != j.end() && it->is_array()) {
            for (const auto& item : *it) {
                auto e = std::make_unique<RelationshipEdge>();
                e->a = item.value("a", "");
                e->b = item.value("b", "");
                e->intimacy = item.value("intimacy", 0.0);
                e->trustAB = item.value("trustAB", 0.5);
                e->trustBA = item.value("trustBA", 0.5);
                edges_.push_back(std::move(e));
            }
        }
    } catch (const std::exception& e) {
        log::warn("RelationshipGraph",
                  std::string("图谱文件解析失败(从空开始): ") + e.what());
        nodes_.clear();
        edges_.clear();
    }
}

void RelationshipGraph::save() {
    nlohmann::json j;
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
        if (!n->attrs.empty()) item["attrs"] = n->attrs;
        nodes.push_back(std::move(item));
    }
    auto edges = nlohmann::json::array();
    for (const auto& e : edges_) {
        edges.push_back({{"a", e->a},
                         {"b", e->b},
                         {"intimacy", e->intimacy},
                         {"trustAB", e->trustAB},
                         {"trustBA", e->trustBA}});
    }
    j["nodes"] = std::move(nodes);
    j["edges"] = std::move(edges);
    std::ofstream out(file_, std::ios::trunc);
    out << j.dump() << '\n';
}

} // namespace mio
