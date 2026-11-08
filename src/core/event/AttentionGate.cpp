#include "core/event/AttentionGate.h"
#include "context/costEstimator/Tokens.h"
#include "log/Log.h"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace mio {

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string previewContent(const std::string& content, std::size_t maxBytes = 60) {
    std::string flat;
    flat.reserve(std::min(content.size(), maxBytes + 16));
    for (char c : content) {
        if (c == '\r' || c == '\n') flat.push_back(' ');
        else flat.push_back(c);
        if (flat.size() >= maxBytes * 2) break;
    }
    return cutUtf8(flat, maxBytes);
}

// 规则匹配优先级：3=匹配 actorId, 2=匹配 channelId, 1=匹配 source, 0=不匹配
int matchScore(const AttentionRule& rule, const Event& ev) {
    const std::string& p = rule.pattern;
    if (p.empty()) return 0;

    if (!ev.actorId.empty() && (p == ev.actorId || p == "user:" + ev.actorId || p == "actor:" + ev.actorId)) {
        return 3;
    }
    if (!ev.channelId.empty() && (p == ev.channelId || p == "channel:" + ev.channelId)) {
        return 2;
    }
    if (!ev.source.empty() && (p == ev.source || p == "source:" + ev.source)) {
        return 1;
    }
    return 0;
}

} // namespace

AttentionGate::AttentionGate(Achieve* achieve) {
    bindAchieve(achieve);
}

void AttentionGate::bindAchieve(Achieve* achieve) {
    std::lock_guard<std::mutex> lock(mu_);
    achieve_ = achieve;
    if (achieve_ && index_ < achieve_->maxId()) {
        index_ = achieve_->maxId();
    }
}

bool AttentionGate::enterQueue(Event inEvt) {
    std::lock_guard<std::mutex> lock(mu_);

    resolveAttention(inEvt);
    if (achieve_) {
        achieve_->append(inEvt);
    }

    std::string actionDesc;
    bool shouldWake = false;
    bool flushDigest = false;

    switch (inEvt.tier) {
    case AttentionTier::DirectFull:
    case AttentionTier::Preview:
        actionDesc = (inEvt.tier == AttentionTier::DirectFull) ? "唤醒意识流" : "预览唤醒";
        shouldWake = true;
        break;

    case AttentionTier::Digest: {
        const std::size_t nextCount = DigestBuffer_.size() + 1;
        if (nextCount >= kDefaultDigestBatchSize) {
            actionDesc = "攒批达到阈值 " + std::to_string(nextCount) + "/" +
                         std::to_string(kDefaultDigestBatchSize) + "，唤醒意识流";
            shouldWake = true;
            flushDigest = true;
        } else {
            actionDesc = "攒批中 " + std::to_string(nextCount) + "/" +
                         std::to_string(kDefaultDigestBatchSize);
            shouldWake = false;
        }
        break;
    }

    case AttentionTier::Silent:
        actionDesc = "静默归档";
        shouldWake = false;
        break;

    default:
        log::error("AttentionGate", "invalid Attention Tier");
        return false;
    }

    // 格式化输出入站事件日志
    std::ostringstream oss;
    oss << "入站事件 #" << inEvt.id << " [" << eventCategoryToString(inEvt.category) << "]";
    if (!inEvt.source.empty()) oss << " [" << inEvt.source << "]";
    if (!inEvt.channelId.empty()) oss << "[" << inEvt.channelId << "]";
    if (!inEvt.title.empty() || !inEvt.actorId.empty()) {
        oss << " 来自:" << (inEvt.title.empty() ? inEvt.actorId : inEvt.title);
        if (!inEvt.title.empty() && !inEvt.actorId.empty() && inEvt.title != inEvt.actorId &&
            inEvt.title.find(inEvt.actorId) == std::string::npos) {
            oss << "(" << inEvt.actorId << ")";
        }
    }
    oss << " tier=" << attentionTierToString(inEvt.tier) << " (" << actionDesc << ")";
    if (!inEvt.content.empty()) {
        oss << ": \"" << previewContent(inEvt.content, 60) << "\"";
    } else if (!inEvt.parts.empty()) {
        oss << " [" << inEvt.parts.size() << " 个多模态附件]";
    }
    log::info("AttentionGate", oss.str());

    // 状态流转入队
    if (inEvt.tier == AttentionTier::DirectFull || inEvt.tier == AttentionTier::Preview) {
        if (!DigestBuffer_.empty()) {
            readyMsgs_.push_back(DigestBuffer2Msg());
        }
        if (auto m = inEvt.toMsg()) {
            readyMsgs_.push_back(std::move(*m));
        }
    } else if (inEvt.tier == AttentionTier::Digest) {
        DigestBuffer_.push_back(std::move(inEvt));
        if (flushDigest) {
            readyMsgs_.push_back(DigestBuffer2Msg());
        }
    }

    return shouldWake;
}

bool AttentionGate::resolveAttention(Event& inEvt) {
    if (inEvt.id == 0) {
        inEvt.id = getIndex();
    }
    if (inEvt.timestamp <= 0) {
        inEvt.timestamp = nowMs();
    }

    const std::int64_t now = nowMs();
    rules_.erase(
        std::remove_if(rules_.begin(), rules_.end(),
                       [now](const AttentionRule& r) { return r.expiresAt > 0 && r.expiresAt <= now; }),
        rules_.end());

    int bestScore = 0;
    const AttentionRule* bestRule = nullptr;
    for (const auto& rule : rules_) {
        int score = matchScore(rule, inEvt);
        if (score >= bestScore && score > 0) {
            bestScore = score;
            bestRule = &rule;
        }
    }

    if (bestRule) {
        inEvt.tier = bestRule->targetTier;
        return true;
    }
    return false;
}

Msg AttentionGate::DigestBuffer2Msg() {
    auto m = Event::toDigestMsg(DigestBuffer_);
    DigestBuffer_.clear();
    if (m) {
        return std::move(*m);
    }
    Msg empty;
    empty.role = Role::User;
    return empty;
}

std::vector<Msg> AttentionGate::drainMsgs(bool flushDigest) {
    std::lock_guard<std::mutex> lock(mu_);
    if (flushDigest && !DigestBuffer_.empty()) {
        readyMsgs_.push_back(DigestBuffer2Msg());
    }
    std::vector<Msg> out;
    out.swap(readyMsgs_);
    return out;
}

void AttentionGate::upsertRule(AttentionRule rule) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& existing : rules_) {
        if (existing.pattern == rule.pattern) {
            existing = std::move(rule);
            return;
        }
    }
    rules_.push_back(std::move(rule));
}

bool AttentionGate::removeRule(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(mu_);
    auto oldSize = rules_.size();
    rules_.erase(
        std::remove_if(rules_.begin(), rules_.end(),
                       [&](const AttentionRule& r) { return r.pattern == pattern; }),
        rules_.end());
    return rules_.size() != oldSize;
}

std::vector<AttentionRule> AttentionGate::listRules() const {
    std::lock_guard<std::mutex> lock(mu_);
    return rules_;
}

} // namespace mio
