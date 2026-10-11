// MemoryManager 实现（见 memory/MemoryManager.h）
//
// 打分模型：score = cos(query, memory) × exp(-age/τ)
//   * 两侧向量都经 L2 归一化 → 点积即余弦，无数量纲干扰；
//   * 时间衰减只影响排序权重，不删除数据（旧记忆仍可被高相似度召回）；
//   * 多线程只用于候选量 ≥ kParallelMinCandidates 时按区间切分打分
//     （各线程写各自的 score 下标，无共享写冲突），小候选集不值得付
//     线程启动成本。

#include "memory/manager/MemoryManager.h"

#include "log/Log.h"
#include "memory/VectorMath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <thread>
#include <utility>

namespace mio {

namespace {

std::string dedupeKey(const std::string& convKey, const std::string& summary) {
    return convKey + '\x1f' + summary;
}

std::string formatDate(std::int64_t epochSeconds) {
    if (epochSeconds <= 0) return "未知时间";
    std::time_t t = static_cast<std::time_t>(epochSeconds);
    std::tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmBuf.tm_year + 1900,
                  tmBuf.tm_mon + 1, tmBuf.tm_mday);
    return buf;
}

} // namespace

MemoryManager::MemoryManager(MemoryConfig cfg, std::shared_ptr<Embedding> embedding,
                             MemoryStore& store)
    : cfg_(cfg), embedding_(std::move(embedding)), store_(store) {}

void MemoryManager::update(MemoryConfig cfg, std::shared_ptr<Embedding> embedding) {
    std::lock_guard<std::mutex> lock(mtx_);
    cfg_ = cfg;
    embedding_ = std::move(embedding);
}

void MemoryManager::updateConfig(MemoryConfig cfg) {
    std::lock_guard<std::mutex> lock(mtx_);
    cfg_ = cfg;
}

void MemoryManager::updateEmbedding(std::shared_ptr<Embedding> embedding) {
    std::lock_guard<std::mutex> lock(mtx_);
    embedding_ = std::move(embedding);
}

bool MemoryManager::embedOnCooldown() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return std::chrono::steady_clock::now() < cooldownUntil_;
}

void MemoryManager::noteEmbedResult(bool ok) const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (ok) {
        lastEmbedFailed_ = false;
        return;
    }
    cooldownUntil_ = std::chrono::steady_clock::now() +
                     std::chrono::seconds(kEmbedCooldownSeconds);
}

std::vector<float> MemoryManager::embedNormalized(
    const std::string& text) const {
    if (embedOnCooldown()) return {};
    std::shared_ptr<Embedding> emb;
    std::size_t expectedDim = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        emb = embedding_;
        expectedDim = cfg_.dim;
    }
    if (!emb) return {};
    try {
        std::vector<float> vec = emb->embed(text);
        noteEmbedResult(true);
        if (vec.size() != expectedDim) {
            log::warn("MemoryManager",
                      "向量维度不符: 期望 " + std::to_string(expectedDim) + " 实际 " +
                          std::to_string(vec.size()) + "（检查 MIO_EMBED_MODEL）");
            return {};
        }
        if (!vecmath::normalizeInPlace(vec)) return {};
        return vec;
    } catch (const std::exception& e) {
        noteEmbedResult(false);
        log::warn("MemoryManager", std::string("向量化失败（进入 ") +
                                         std::to_string(kEmbedCooldownSeconds) +
                                         "s 冷却）: " + e.what());
        return {};
    }
}

void MemoryManager::remember(const ConversationKey& conv,
                             const std::string& personId,
                             const std::string& summary, bool isPublic,
                             std::int64_t now) {
    if (summary.empty()) return;

    // 进程内幂等：同一摘要文本（同一会话）只写一次。
    // 跨进程幂等由 UNIQUE(conv_key, summary) + INSERT OR IGNORE 保证。
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const std::string key = dedupeKey(conv.toString(), summary);
        if (!remembered_.insert(key).second) return;
        if (remembered_.size() > kRememberedCap) remembered_.clear();
    }

    const std::vector<float> vec = embedNormalized(summary);
    if (vec.empty()) return;  // 端点不可用：本次写入放弃（不回滚去重集合也安全）

    MemoryRecord record;
    record.convKey = conv.toString();
    record.personId = personId;
    record.isPublic = isPublic;
    record.createdAt = now;
    record.summary = summary;
    try {
        const bool inserted =
            store_.insert(record, vec.data(), vec.size());
        if (inserted)
            log::info("MemoryManager", "记忆已写入: conv=" + record.convKey +
                                           " person=" + personId +
                                           (isPublic ? " 公开" : " 私密"));
    } catch (const std::exception& e) {
        log::warn("MemoryManager", std::string("记忆写入失败: ") + e.what());
    }
}

std::vector<RecalledMemory> MemoryManager::recall(
    const std::string& query, const std::string& viewerPersonId,
    const std::string& convKey, std::int64_t now, std::size_t topK) {
    if (topK == 0) topK = cfg_.topK;
    if (query.empty() || topK == 0) return {};

    const std::vector<float> qvec = embedNormalized(query);
    if (qvec.empty()) return {};

    std::vector<MemoryCandidate> candidates;
    try {
        MemoryFilter filter;
        filter.viewerPersonId = viewerPersonId;
        filter.convKey = convKey;
        filter.limit = cfg_.maxCandidates;
        candidates = store_.selectCandidates(filter);
    } catch (const std::exception& e) {
        log::warn("MemoryManager", std::string("记忆候选集查询失败: ") + e.what());
        return {};
    }

    // SIMD 点积 + 时间衰减（候选量过阈值时多线程切分）
    const std::size_t n = candidates.size();
    std::vector<double> scores(n, 0.0);
    const unsigned hw = std::thread::hardware_concurrency();
    if (n >= kParallelMinCandidates && hw > 1) {
        const std::size_t nThreads =
            std::min<std::size_t>(hw, 4);
        std::vector<std::thread> pool;
        const std::size_t chunk = (n + nThreads - 1) / nThreads;
        for (std::size_t t = 0; t < nThreads; ++t) {
            pool.emplace_back([&, t, chunk] {
                const std::size_t begin = t * chunk;
                const std::size_t end = std::min(n, begin + chunk);
                for (std::size_t i = begin; i < end; ++i)
                    scores[i] = vecmath::dot(qvec.data(), candidates[i].embedding.data(),
                                             qvec.size());
            });
        }
        for (auto& th : pool) th.join();
    } else {
        for (std::size_t i = 0; i < n; ++i)
            scores[i] =
                vecmath::dot(qvec.data(), candidates[i].embedding.data(), qvec.size());
    }

    // 过滤（相似度门槛）+ 时间衰减打分
    std::vector<RecalledMemory> hits;
    hits.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double sim = scores[i];
        if (!(sim >= cfg_.minSimilarity)) continue;
        const std::int64_t age =
            std::max<std::int64_t>(0, now - candidates[i].record.createdAt);
        const double decay = std::exp(-static_cast<double>(age) /
                                      cfg_.decayTauSeconds);
        RecalledMemory r;
        r.id = candidates[i].record.id;
        r.convKey = std::move(candidates[i].record.convKey);
        r.personId = std::move(candidates[i].record.personId);
        r.isPublic = candidates[i].record.isPublic;
        r.createdAt = candidates[i].record.createdAt;
        r.summary = std::move(candidates[i].record.summary);
        r.similarity = sim;
        r.score = sim * decay;
        hits.push_back(std::move(r));
    }

    if (hits.size() > topK) {
        std::partial_sort(hits.begin(), hits.begin() + static_cast<long>(topK),
                          hits.end(),
                          [](const RecalledMemory& a, const RecalledMemory& b) {
                              return a.score > b.score;
                          });
        hits.resize(topK);
    }
    if (!hits.empty())
        log::debug("MemoryManager", "召回 " + std::to_string(hits.size()) + "/" +
                                        std::to_string(n) + " 条记忆（候选集 SQL 预过滤）");
    return hits;
}

std::size_t MemoryManager::count() {
    try {
        return store_.count();
    } catch (...) {
        return 0;
    }
}

std::string MemoryManager::renderForPrompt(
    const std::vector<RecalledMemory>& recalled) {
    if (recalled.empty()) return "";
    std::string out =
        "[长期记忆召回] 以下是与当前话题相关的历史经历摘要（按相关度排序，"
        "供自然引用；不要虚构未列出的记忆）：\n";
    for (const auto& r : recalled) {
        out += "- [" + formatDate(r.createdAt) +
               (r.isPublic ? " · 公开经历" : " · 私密经历") + "] " + r.summary + "\n";
    }
    return out;
}

} // namespace mio
