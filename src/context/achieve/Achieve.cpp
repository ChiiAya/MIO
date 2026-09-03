// Achieve 实现（见 context/achieve/Achieve.h）

#include "context/achieve/Achieve.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

#include "log/Log.h"

namespace mio {

Achieve::Achieve(std::filesystem::path dir) : dir_(std::move(dir)) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec)
        throw std::runtime_error("无法创建档案目录 " + dir_.string() + ": " +
                                 ec.message());
}

std::filesystem::path Achieve::filePathFor(const std::filesystem::path& dir,
                                           const ConversationKey& key) {
    // key 可能含平台侧 id，做一次文件名消毒（只留字母数字 _ . -）
    std::string safe;
    for (char c : key.toString()) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
            c == '.')
            safe += c;
        else
            safe += '_';
    }
    return dir / (safe + ".jsonl");
}

std::vector<Msg>& Achieve::ensureLoaded(const ConversationKey& key) {
    auto& msgs = cache_[key.toString()];
    if (!msgs.empty()) return msgs;

    std::ifstream in(filePathFor(dir_, key));
    if (!in) return msgs;  // 首次对话：空历史

    std::string line;
    try {
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            msgs.push_back(msgFromJson(nlohmann::json::parse(line)));
        }
    } catch (const std::exception& e) {
        log::warn("Achieve",
                  std::string("档案解析失败(从已读部分继续): ") + e.what());
    }
    return msgs;
}

std::vector<Msg> Achieve::load(const ConversationKey& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    return ensureLoaded(key);
}

ColdRead Achieve::coldRead(const ConversationKey& key, SummaryManager& summary,
                           int summaryCount, int rawKeep) {
    std::lock_guard<std::mutex> lock(mtx_);
    const std::vector<Msg>& full = ensureLoaded(key);
    const std::size_t n = full.size();

    ColdRead out;
    if (n <= static_cast<std::size_t>(rawKeep)) {
        out.msgs = full;  // 不足 rawKeep 条：原文返回，不调 LLM（保持默认私密）
        return out;
    }
    // 摘要前 summaryCount 条（不与近 rawKeep 重叠），近 rawKeep 条原文保留
    const std::size_t summarizeN =
        std::min<std::size_t>(static_cast<std::size_t>(summaryCount),
                              n - static_cast<std::size_t>(rawKeep));
    std::vector<Msg> toSummarize(full.begin(), full.begin() + summarizeN);
    const SummaryOutcome outcome = summary.summarizeWithVerdict(toSummarize);

    Msg summaryMsg;
    summaryMsg.role = Role::User;
    summaryMsg.text = outcome.text;
    out.msgs.push_back(std::move(summaryMsg));
    out.msgs.insert(out.msgs.end(), full.end() - rawKeep, full.end());
    // privateVerdict = true 表示私密，isPublic 取反；
    // topic 随摘要同步带出，供调用方创建/重建 unit 时回写。
    out.isPublic = !outcome.privateVerdict;
    out.topic = outcome.topic;
    return out;
}

void Achieve::append(const ConversationKey& key, const Msg& msg) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto& msgs = ensureLoaded(key);
    msgs.push_back(msg);
    std::ofstream out(filePathFor(dir_, key), std::ios::app);
    out << msgToJson(msg).dump() << '\n';
}

void Achieve::append(const ConversationKey& key, const std::vector<Msg>& msgs) {
    for (const auto& m : msgs) append(key, m);
}

std::size_t Achieve::count(const ConversationKey& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    return ensureLoaded(key).size();
}

} // namespace mio
