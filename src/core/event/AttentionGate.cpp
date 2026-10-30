#include "core/event/AttentionGate.h"
#include "log/Log.h"

#include <algorithm>
#include <chrono>

namespace mio {

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
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

    switch (inEvt.tier) {
    case AttentionTier::DirectFull:
    case AttentionTier::Preview:
        if (!DigestBuffer_.empty()) {
            readyMsgs_.push_back(DigestBuffer2Msg());
        }
        if (auto m = inEvt.toMsg()) {
            readyMsgs_.push_back(std::move(*m));
        }
        return true;

    case AttentionTier::Digest:
        DigestBuffer_.push_back(std::move(inEvt));
        if (DigestBuffer_.size() >= kDefaultDigestBatchSize) {
            readyMsgs_.push_back(DigestBuffer2Msg());
            return true;
        }
        return false;

    case AttentionTier::Silent:
        return false;

    default:
        log::error("AttentionGate", "invalid Attention Tier");
        return false;
    }
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
