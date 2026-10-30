#include "context/achieve/Achieve.h"
#include "log/Log.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mio {

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

Achieve::Achieve(std::filesystem::path dir, AchieveConfig cfg)
    : dir_(std::move(dir)), filePath_(dir_ / "events.jsonl"), cfg_(cfg) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    std::filesystem::create_directories(dir_ / "media", ec);
    if (ec) {
        throw std::runtime_error("无法创建事件档案目录 " + dir_.string() + ": " + ec.message());
    }
    loadFromDisk();
}

void Achieve::setConfig(AchieveConfig cfg) {
    std::lock_guard<std::mutex> lock(mtx_);
    cfg_ = cfg;
}

AchieveConfig Achieve::config() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return cfg_;
}

std::int64_t Achieve::normalizeToMs(std::int64_t ts) {
    if (ts <= 0) return 0;
    return (ts < 100000000000LL) ? (ts * 1000) : ts;
}

bool Achieve::matchesTarget(const Event& ev, const std::string& target) {
    if (target.empty()) return true;
    return ev.channelId == target || ev.source == target || ev.actorId == target ||
           (ev.source + ":" + ev.channelId) == target;
}

void Achieve::offloadInlineMedia(Event& ev) const {
    if (cfg_.inlineMediaMaxBytes == 0) return;
    for (std::size_t i = 0; i < ev.parts.size(); ++i) {
        auto& p = ev.parts[i];
        if (p.data.size() > cfg_.inlineMediaMaxBytes && p.data.rfind("data:", 0) == 0) {
            const std::string fileName =
                "ev_" + std::to_string(ev.id) + "_" + std::to_string(i) + ".uri";
            const auto mediaPath = dir_ / "media" / fileName;
            std::ofstream out(mediaPath, std::ios::binary);
            if (out) {
                out << p.data;
                p.data = "file://" + mediaPath.string();
            }
        }
    }
}

void Achieve::loadFromDisk() {
    std::ifstream in(filePath_);
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            Event ev = Event::fromJson(nlohmann::json::parse(line));
            ev.timestamp = normalizeToMs(ev.timestamp);
            const std::size_t idx = events_.size();
            events_.push_back(std::move(ev));
            const std::uint64_t evId = events_.back().id;
            if (evId > 0) {
                idIndex_[evId] = idx;
                maxId_ = std::max(maxId_, evId);
            }
        } catch (const std::exception& e) {
            log::warn("Achieve", std::string("跳过损坏的事件记录: ") + e.what());
        }
    }
}

void Achieve::append(Event ev) {
    std::lock_guard<std::mutex> lock(mtx_);
    ev.timestamp = normalizeToMs(ev.timestamp);
    offloadInlineMedia(ev);

    const std::size_t idx = events_.size();
    events_.push_back(ev);

    if (ev.id > 0) {
        idIndex_[ev.id] = idx;
        maxId_ = std::max(maxId_, ev.id);
    }

    std::ofstream out(filePath_, std::ios::app);
    out << ev.toJson().dump() << '\n';
}

void Achieve::append(const std::vector<Event>& events) {
    for (const auto& ev : events) {
        append(ev);
    }
}

std::vector<Event> Achieve::queryByTime(const std::string& target,
                                        std::int64_t startTime,
                                        std::int64_t endTime,
                                        std::size_t limit) {
    std::lock_guard<std::mutex> lock(mtx_);
    const std::int64_t startMs = normalizeToMs(startTime);
    const std::int64_t endMs = endTime > 0 ? normalizeToMs(endTime) : nowMs();

    std::vector<Event> matched;
    for (const auto& ev : events_) {
        if (startMs > 0 && ev.timestamp < startMs) continue;
        if (endMs > 0 && ev.timestamp > endMs) continue;
        if (!matchesTarget(ev, target)) continue;
        matched.push_back(ev);
    }

    if (limit > 0 && matched.size() > limit) {
        matched.erase(matched.begin(), matched.end() - static_cast<std::ptrdiff_t>(limit));
    }
    return matched;
}

std::vector<Event> Achieve::queryRecentSeconds(const std::string& target,
                                               std::int64_t durationSeconds,
                                               std::size_t limit) {
    const std::int64_t endMs = nowMs();
    const std::int64_t startMs = durationSeconds > 0 ? (endMs - durationSeconds * 1000) : 0;
    return queryByTime(target, startMs, endMs, limit);
}

std::optional<Event> Achieve::getById(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = idIndex_.find(id);
    if (it == idIndex_.end() || it->second >= events_.size()) {
        return std::nullopt;
    }
    return events_[it->second];
}

std::string Achieve::formatEventsForTool(const std::vector<Event>& events) {
    if (events.empty()) {
        return "[无匹配事件记录]";
    }
    std::ostringstream oss;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (auto m = events[i].toMsg(AttentionTier::DirectFull)) {
            if (i > 0) oss << "\n";
            oss << m->text;
        }
    }
    return oss.str();
}

ColdRead Achieve::coldRead(const std::string& target, SummaryManager* summary,
                           int summaryCountOverride, int rawKeepOverride) {
    AchieveConfig cfgCopy;
    std::vector<Event> matched;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cfgCopy = cfg_;
        for (const auto& ev : events_) {
            if (matchesTarget(ev, target)) {
                matched.push_back(ev);
            }
        }
    }

    ColdRead out;
    if (!cfgCopy.enableColdSummary) {
        return out;
    }

    const int sumCnt = summaryCountOverride >= 0 ? summaryCountOverride : cfgCopy.summaryCount;
    const int keepCnt = rawKeepOverride >= 0 ? rawKeepOverride : cfgCopy.rawKeep;
    const std::size_t rawKeep = static_cast<std::size_t>(std::max(keepCnt, 0));
    const std::size_t summaryCount = static_cast<std::size_t>(std::max(sumCnt, 0));

    const std::size_t windowSize = summaryCount + rawKeep;
    if (windowSize > 0 && matched.size() > windowSize) {
        matched.erase(matched.begin(), matched.end() - static_cast<std::ptrdiff_t>(windowSize));
    }

    std::vector<Msg> fullMsgs;
    fullMsgs.reserve(matched.size());
    for (const auto& ev : matched) {
        if (auto m = ev.toMsg(AttentionTier::DirectFull)) {
            fullMsgs.push_back(std::move(*m));
        }
    }

    if (fullMsgs.size() <= rawKeep || summaryCount == 0 || summary == nullptr) {
        out.msgs = std::move(fullMsgs);
        return out;
    }

    const std::size_t tailStart = fullMsgs.size() - rawKeep;
    std::vector<Msg> toSummarize(fullMsgs.begin(), fullMsgs.begin() + static_cast<std::ptrdiff_t>(tailStart));

    try {
        SummaryOutcome outcome = summary->summarizeWithVerdict(toSummarize);
        if (outcome.text.empty()) throw std::runtime_error("摘要为空");

        Msg summaryMsg;
        summaryMsg.role = Role::User;
        summaryMsg.text = "[历史事件摘要] " + outcome.text;
        out.msgs.push_back(std::move(summaryMsg));
        out.msgs.insert(out.msgs.end(),
                        fullMsgs.begin() + static_cast<std::ptrdiff_t>(tailStart),
                        fullMsgs.end());
        out.topic = outcome.topic;
    } catch (const std::exception& e) {
        log::warn("Achieve", std::string("冷读取摘要失败，退回原文尾部: ") + e.what());
        out.msgs.assign(fullMsgs.begin() + static_cast<std::ptrdiff_t>(tailStart), fullMsgs.end());
    }
    return out;
}

std::uint64_t Achieve::maxId() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return maxId_;
}

} // namespace mio
