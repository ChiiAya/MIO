
#include "diary/Diary.h"

#include <algorithm>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "log/Log.h"

namespace mio {

namespace {

nlohmann::json entryToJson(const DiaryEntry& e) {
    return {{"seq", e.seq}, {"createdAt", e.createdAt}, {"text", e.text}};
}

DiaryEntry entryFromJson(const nlohmann::json& j) {
    DiaryEntry e;
    e.seq = j.value("seq", std::uint64_t{0});
    e.createdAt = j.value("createdAt", std::int64_t{0});
    e.text = j.value("text", "");
    return e;
}

} // namespace

Diary::Diary(std::filesystem::path file) : file_(std::move(file)) { load(); }

void Diary::load() {
    std::ifstream in(file_);
    if (!in) return;
    try {
        nlohmann::json j = nlohmann::json::parse(in);
        nextSeq_ = j.value("nextSeq", std::uint64_t{1});
        if (auto it = j.find("pending"); it != j.end() && it->is_array())
            for (const auto& item : *it) pending_.push_back(entryFromJson(item));
        if (auto it = j.find("archived"); it != j.end() && it->is_array())
            for (const auto& item : *it) archived_.push_back(entryFromJson(item));
    } catch (const std::exception& e) {
        log::warn("Diary", std::string("日记解析失败(从空继续): ") + e.what());
    }
}

void Diary::saveLocked() {
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    nlohmann::json j;
    j["nextSeq"] = nextSeq_;
    auto p = nlohmann::json::array();
    for (const auto& e : pending_) p.push_back(entryToJson(e));
    auto a = nlohmann::json::array();
    for (const auto& e : archived_) a.push_back(entryToJson(e));
    j["pending"] = std::move(p);
    j["archived"] = std::move(a);
    std::ofstream out(file_);
    out << j.dump(2) << '\n';
}

void Diary::save() { saveLocked(); }

void Diary::add(const std::string& text, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mtx_);
    pending_.push_back({nextSeq_++, now, text});
    // 上限兜底：丢弃最旧未消费条目
    while (pending_.size() > maxPending_) pending_.erase(pending_.begin());
    saveLocked();
}

std::vector<DiaryEntry> Diary::consume() {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<DiaryEntry> out = std::move(pending_);
    pending_.clear();
    for (const auto& e : out) archived_.push_back(e);  // 封存留档
    if (!out.empty()) saveLocked();
    return out;
}

std::size_t Diary::pendingCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return pending_.size();
}

} // namespace mio
