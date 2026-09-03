#include "core/eventlog/EventLog.h"

#include <stdexcept>

#include "log/Log.h"

namespace mio {

EventLog::EventLog(std::filesystem::path file) : file_(std::move(file)) {
    const auto parent = file_.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
            throw std::runtime_error("无法创建事件日志目录 " + parent.string() +
                                     ": " + ec.message());
    }
    // 启动：从尾部恢复 seq（只读最后一行，不加载全部）
    {
        std::ifstream in(file_);
        std::string line, last;
        while (std::getline(in, line))
            if (!line.empty()) last = line;
        if (!last.empty()) {
            try {
                lastSeq_ = nlohmann::json::parse(last).value(
                    "seq", std::uint64_t{0});
            } catch (const std::exception&) {
            }
        }
    }
    out_.open(file_, std::ios::app);
    if (!out_)
        throw std::runtime_error("无法打开事件日志: " + file_.string());
}

Event EventLog::append(Event ev) {
    std::lock_guard<std::mutex> lock(mtx_);
    ev.seq = ++lastSeq_;
    out_ << eventToJson(ev).dump() << '\n';
    out_.flush();
    if (!out_)
        throw std::runtime_error("事件日志写入失败: " + file_.string());
    return ev;
}

} // namespace mio
