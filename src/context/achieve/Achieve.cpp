// Achieve 实现（见 context/achieve/Achieve.h）
//
// 线程模型：所有公开方法持 mtx_ 完成"取快照/写文件"，返回值拷贝；
// 本类内部不调用任何模型或插件，因此不存在"持锁调用外部组件"的路径。
// coldStart/readRange 等返回【值拷贝】，调用方拿到结果时锁已释放。

#include "context/achieve/Achieve.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

#include "log/Log.h"

#include <nlohmann/json.hpp>

namespace mio {

namespace {

constexpr int kIndexVersion = 1;
constexpr int kRegistryVersion = 1;
// 旁路注册表（不是档案）：启动枚举用的"会话键 → 档案文件"可重建索引
constexpr const char* kRegistryFileName = "_conversations.json";
// 冷启动候选窗口：只投影尾部这么多条记录，避免大档案全量拷贝。
// maxMessages 只影响可选条数，工具回合会消耗窗口，所以留 8 倍余量 + 下限。
constexpr std::size_t kColdStartWindowMin = 256;

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isHexLower(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string sanitizeLegacyName(const std::string& s) {
    // 旧实现：非 [A-Za-z0-9._-] 一律替换成 '_'（会碰撞，故只用于兼容读取）
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) != 0 || c == '_' || c == '-' || c == '.')
            out += c;
        else
            out += '_';
    }
    return out;
}

std::string toLowerAscii(const std::string& s) {
    // 只折叠 ASCII：UTF-8 多字节序列按字节比较（中文等无大小写概念，
    // 拉丁扩展字符不做 Unicode case folding —— 已知局限，见报告）
    std::string out = s;
    for (char& c : out) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x80) c = static_cast<char>(std::tolower(uc));
    }
    return out;
}

std::string fnv1aHex8(const std::string& s) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    static const char* digits = "0123456789abcdef";
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[h & 0x0F];
        h >>= 4;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// 路径编码：防会话键碰撞
// ---------------------------------------------------------------------------
std::string Achieve::encodeKey(const std::string& convKey) {
    // 百分号编码每个非 [A-Za-z0-9._-] 字节 —— 可逆、不依赖"替换特殊字符"
    // （替换会让 "a/b" 与 "a_b" 撞同一文件）。再追加 FNV-1a 短哈希，
    // 使编码被长度上限截断后仍能区分不同会话键。
    static const char* digits = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(convKey.size() + 16);
    for (unsigned char c : convKey) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                          c == '-';
        if (safe) {
            escaped += static_cast<char>(c);
        } else {
            escaped += '%';
            escaped += digits[c >> 4];
            escaped += digits[c & 0x0F];
        }
    }
    // 文件名长度上限（多数文件系统 255 字节）：超长会话键截断编码前缀，
    // 由短哈希保证唯一性；截断处不能切开 "%XX"
    constexpr std::size_t kMaxEscaped = 96;
    if (escaped.size() > kMaxEscaped) {
        std::size_t cut = kMaxEscaped;
        // 只切在转义单元边界：%XY 是 3 字节，末位是 '%' 丢 1 字节，
        // 末二位是 '%' 丢 2 字节（'%' 只可能作为转义起始出现）
        if (cut > 0 && escaped[cut - 1] == '%')
            cut -= 1;
        else if (cut > 1 && escaped[cut - 2] == '%')
            cut -= 2;
        escaped.resize(cut);
    }
    return escaped + "-" + fnv1aHex8(convKey);
}

std::filesystem::path Achieve::primaryPathFor(const std::filesystem::path& dir,
                                              const std::string& convKey) {
    return dir / (encodeKey(convKey) + ".jsonl");
}

std::filesystem::path Achieve::primaryPathFor(const std::filesystem::path& dir,
                                              const ConversationKey& key) {
    return primaryPathFor(dir, key.toString());
}

std::vector<std::string> Achieve::legacyFileNames(const std::string& convKey) {
    std::vector<std::string> names;
    auto add = [&names](const std::string& n) {
        if (std::find(names.begin(), names.end(), n) == names.end())
            names.push_back(n);
    };
    add(sanitizeLegacyName(convKey) + ".jsonl");  // 现行旧方案（toString 直接消毒）
    // 更早的方案：档案名不带平台前缀（private_0d00.jsonl / group_10001.jsonl）
    for (const char* scope : {"private:", "group:"}) {
        const auto pos = convKey.rfind(scope);
        if (pos != std::string::npos)
            add(sanitizeLegacyName(convKey.substr(pos)) + ".jsonl");
    }
    return names;
}

std::filesystem::path Achieve::resolvePathFor(const std::filesystem::path& dir,
                                              const std::string& convKey) {
    std::error_code ec;
    const auto primary = primaryPathFor(dir, convKey);
    if (std::filesystem::exists(primary, ec) && !ec) return primary;
    for (const auto& name : legacyFileNames(convKey)) {
        const auto p = dir / name;
        if (std::filesystem::exists(p, ec) && !ec) return p;  // 读到旧文件就用它
    }
    return primary;  // 新会话：使用新命名（稳定，不随启动变化）
}

const char* Achieve::registryFileName() { return kRegistryFileName; }

bool Achieve::decodeKeyStem(const std::string& stem, std::string& out) {
    // 新命名 = <百分号编码前缀>-<8位十六进制 FNV-1a>
    out.clear();
    const auto dash = stem.rfind('-');
    if (dash == std::string::npos || dash == 0) return false;
    const std::string hash = stem.substr(dash + 1);
    if (hash.size() != 8) return false;
    for (char c : hash)
        if (!isHexLower(c)) return false;

    const std::string escaped = stem.substr(0, dash);
    std::string decoded;
    decoded.reserve(escaped.size());
    for (std::size_t i = 0; i < escaped.size();) {
        const char c = escaped[i];
        if (c == '%') {
            if (i + 2 >= escaped.size()) return false;
            const int hi = hexVal(escaped[i + 1]);
            const int lo = hexVal(escaped[i + 2]);
            if (hi < 0 || lo < 0) return false;
            decoded += static_cast<char>((hi << 4) | lo);
            i += 3;
            continue;
        }
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!safe) return false;
        decoded += c;
        ++i;
    }
    if (decoded.empty()) return false;
    out = std::move(decoded);
    return true;
}

// ---------------------------------------------------------------------------
// 加载与旁路索引
// ---------------------------------------------------------------------------
Achieve::Achieve(std::filesystem::path dir) : dir_(std::move(dir)) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec)
        throw std::runtime_error("无法创建档案目录 " + dir_.string() + ": " +
                                 ec.message());
}

Achieve::Conversation& Achieve::ensureLoadedLocked(const std::string& convKey) const {
    auto it = cache_.find(convKey);
    if (it != cache_.end() && it->second.loaded) return it->second;

    Conversation conv;
    auto pit = resolvedPaths_.find(convKey);
    if (pit != resolvedPaths_.end()) {
        conv.path = pit->second;
    } else {
        conv.path = resolvePathFor(dir_, convKey);
        resolvedPaths_.emplace(convKey, conv.path);
    }
    if (conv.path.empty()) conv.path = primaryPathFor(dir_, convKey);
    loadFileLocked(convKey, conv);
    conv.loaded = true;
    auto ins = cache_.emplace(convKey, std::move(conv));
    writeIndexLocked(convKey, ins.first->second);
    return ins.first->second;
}

void Achieve::loadFileLocked(const std::string& convKey,
                             Conversation& conv) const {
    std::error_code ec;
    if (!std::filesystem::exists(conv.path, ec) || ec) return;  // 首次对话：空历史

    std::ifstream in(conv.path, std::ios::binary);
    if (!in) {
        // 存在但打不开（权限、同名目录…）：必须可观察，不能当成"没有历史"
        noteDamageLocked(convKey, conv.path.string(), 0,
                         "无法打开档案文件（存在但不是可读普通文件）");
        return;
    }

    std::string line;
    std::int64_t lineNo = 0;
    std::int64_t recIndex = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (line.empty()) continue;  // 空行不占记录位置（尾随换行不算一条）
        ++recIndex;

        Msg msg;
        bool parsed = false;
        std::string reason;
        try {
            const auto j = nlohmann::json::parse(line);
            if (!j.is_object()) {
                reason = "行不是 JSON 对象";
            } else {
                msg = msgFromJson(j);
                parsed = true;
            }
        } catch (const std::exception& e) {
            reason = std::string("JSON 解析失败: ") + e.what();
        }

        // 稳定 ID：显式 ID 优先（且必须递增），其余按【行位置】补 ——
        // 纯旧档案得到 1..N；损坏行也占一个位置，避免后续 ID 塌陷；
        // 该规则只依赖文件内容，删掉索引重建结果完全一致。
        std::int64_t id = 0;
        if (parsed && msg.messageId > conv.maxId)
            id = msg.messageId;
        else
            id = std::max(conv.maxId + 1, recIndex);
        conv.maxId = id;
        conv.lineIds.push_back(id);

        if (parsed) {
            msg.messageId = id;
            conv.msgs.push_back(std::move(msg));
        } else {
            // 损坏行报告位置与原因；后续行继续读出并各自分配 ID
            noteDamageLocked(convKey, conv.path.string(), lineNo, reason);
            log::warn("Achieve", "档案损坏行: " + conv.path.string() + ":" +
                                     std::to_string(lineNo) + " " + reason);
        }
    }
}

void Achieve::noteDamageLocked(const std::string& convKey,
                               const std::string& path, std::int64_t lineNumber,
                               const std::string& reason) const {
    for (const auto& d : damage_) {
        if (d.filePath == path && d.lineNumber == lineNumber) return;  // 去重
    }
    ArchiveDamage d;
    d.conversationKey = convKey;
    d.filePath = path;
    d.lineNumber = lineNumber;
    d.reason = reason;
    damage_.push_back(std::move(d));
}

void Achieve::writeIndexLocked(const std::string& convKey,
                               const Conversation& conv) const {
    if (conv.path.empty()) return;
    std::error_code ec;
    const auto size = std::filesystem::file_size(conv.path, ec);
    if (ec) return;  // 文件不存在/不可 stat（例如同名目录）：不写索引

    // 档案确实存在 → 登记到旁路注册表（key → 文件名），供启动枚举；
    // 注册表丢失/写失败都不影响读档案（下次按 key 解析路径即可重建）
    registerConversationLocked(convKey, conv.path);

    const std::string idxPath = conv.path.string() + ".idx.json";
    // 索引只是可重建的旁路快照：fileSize + lineCount 一致就不重写
    {
        std::ifstream old(idxPath, std::ios::binary);
        if (old) {
            try {
                nlohmann::json j;
                old >> j;
                if (j.value("version", 0) == kIndexVersion &&
                    j.value("fileSize", std::uint64_t{0}) ==
                        static_cast<std::uint64_t>(size) &&
                    j.value("lineCount", std::size_t{0}) == conv.lineIds.size())
                    return;
            } catch (const std::exception&) {
                // 索引损坏：直接重建（ID 不依赖索引）
            }
        }
    }

    nlohmann::json j;
    j["version"] = kIndexVersion;
    j["conversationKey"] = convKey;
    j["filePath"] = conv.path.string();
    j["fileSize"] = static_cast<std::uint64_t>(size);
    j["lineCount"] = conv.lineIds.size();
    j["nextMessageId"] = conv.maxId + 1;
    j["ids"] = conv.lineIds;  // 行位置 → messageId 映射（含损坏行占位）
    j["updatedAt"] = static_cast<std::int64_t>(std::time(nullptr));

    const std::string tmpPath = idxPath + ".tmp";
    std::ofstream out(tmpPath, std::ios::trunc | std::ios::binary);
    if (!out) {
        // 索引写失败不影响已落盘事实（只读目录也能正常读档案）
        log::debug("Achieve", "旁路索引不可写（忽略）: " + idxPath);
        return;
    }
    out << j.dump();
    out.flush();
    if (!out.good()) {
        out.close();
        std::filesystem::remove(tmpPath, ec);
        log::warn("Achieve", "旁路索引写入失败（忽略）: " + idxPath);
        return;
    }
    out.close();
    std::filesystem::rename(tmpPath, idxPath, ec);
    if (ec) {
        std::filesystem::remove(idxPath, ec);
        std::filesystem::rename(tmpPath, idxPath, ec);
        if (ec) std::filesystem::remove(tmpPath, ec);
    }
}

// ---------------------------------------------------------------------------
// 旁路注册表 + 启动枚举（只读；读档案不依赖注册表）
// ---------------------------------------------------------------------------
void Achieve::ensureRegistryLoadedLocked() const {
    if (registryLoaded_) return;
    registryLoaded_ = true;  // 读失败也置位：不反复读盘、不在枚举时创建文件
    std::ifstream in(dir_ / kRegistryFileName, std::ios::binary);
    if (!in) return;
    try {
        nlohmann::json j;
        in >> j;
        if (j.value("version", 0) != kRegistryVersion) {
            log::warn("Achieve",
                      std::string("旁路注册表版本不认识（忽略，可按会话消息重建）: ") +
                          kRegistryFileName);
            return;
        }
        for (const auto& item : j.value("conversations", nlohmann::json::array())) {
            const std::string key = item.value("key", "");
            const std::string file = item.value("file", "");
            if (!key.empty() && !file.empty()) registry_[key] = file;
        }
    } catch (const std::exception& e) {
        log::warn("Achieve", std::string("旁路注册表损坏（忽略，可按会话消息重建）: ") +
                                 e.what());
    }
}

void Achieve::writeRegistryLocked() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [key, file] : registry_)
        arr.push_back({{"key", key}, {"file", file}});
    nlohmann::json j;
    j["version"] = kRegistryVersion;
    j["updatedAt"] = static_cast<std::int64_t>(std::time(nullptr));
    j["conversations"] = std::move(arr);

    const std::string path = (dir_ / kRegistryFileName).string();
    const std::string tmpPath = path + ".tmp";
    std::error_code ec;
    std::ofstream out(tmpPath, std::ios::trunc | std::ios::binary);
    if (!out) {
        // 只读目录也能正常读写档案：注册表只是加速枚举，失败可接受
        log::debug("Achieve", "旁路注册表不可写（忽略）: " + path);
        return;
    }
    out << j.dump();
    out.flush();
    if (!out.good()) {
        out.close();
        std::filesystem::remove(tmpPath, ec);
        log::warn("Achieve", "旁路注册表写入失败（忽略）: " + path);
        return;
    }
    out.close();
    std::filesystem::rename(tmpPath, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmpPath, path, ec);
        if (ec) std::filesystem::remove(tmpPath, ec);
    }
}

void Achieve::registerConversationLocked(const std::string& convKey,
                                          const std::filesystem::path& path) const {
    if (convKey.empty() || path.empty()) return;
    ensureRegistryLoadedLocked();
    const std::string file = path.filename().string();
    if (file.empty()) return;
    auto it = registry_.find(convKey);
    if (it != registry_.end() && it->second == file) return;  // 无变化不重写
    registry_[convKey] = file;
    writeRegistryLocked();
}

std::vector<std::string> Achieve::scanArchivesLocked(
    std::vector<ArchiveDiscoveryDiagnostic>* diagnostics) const {
    std::vector<std::string> keys;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir_, ec) || ec) return keys;

    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (ec) break;
        std::error_code fec;
        if (!entry.is_regular_file(fec) || fec) continue;
        const std::string name = entry.path().filename().string();
        // 只认 .jsonl 档案：索引（*.jsonl.idx.json）、临时文件、注册表都不是档案
        if (!endsWith(name, ".jsonl") || endsWith(name, ".tmp")) continue;

        const std::string stem = name.substr(0, name.size() - 6);
        std::string key;
        if (decodeKeyStem(stem, key) && encodeKey(key) == stem) {
            keys.push_back(std::move(key));  // 新命名：往返校验通过才采信
            continue;
        }
        // 旧命名（private_0d00.jsonl / consoleprivate_0d00.jsonl 等）无法无损
        // 反解出带平台的真实会话键：不猜 key，记录诊断并跳过（不抛异常）
        if (diagnostics != nullptr) {
            std::string reason =
                "旧命名档案无法无损反解会话键（可用旁路注册表或收到该会话消息后重建）";
            bool dup = false;
            for (const auto& d : *diagnostics)
                if (d.fileName == name && d.reason == reason) dup = true;
            if (!dup) diagnostics->push_back({name, reason});
        }
    }
    return keys;
}

std::vector<std::string> Achieve::conversationKeys() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::set<std::string> keys;  // set → 输出按 key 排序且稳定

    // 1) 旁路注册表（惰性读盘；丢失/损坏都不影响下面两步）
    ensureRegistryLoadedLocked();
    for (const auto& [key, file] : registry_) {
        std::error_code ec;
        if (std::filesystem::exists(dir_ / file, ec) && !ec) keys.insert(key);
    }
    // 2) 本进程已解析过的会话（注册表不可写时的兜底，例如只读目录）
    for (const auto& [key, path] : resolvedPaths_) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec) keys.insert(key);
    }
    // 3) 目录扫描补充（新命名往返校验；旧命名只记诊断）
    for (auto& key : scanArchivesLocked(nullptr)) keys.insert(std::move(key));

    return std::vector<std::string>(keys.begin(), keys.end());
}

std::vector<ArchiveDiscoveryDiagnostic> Achieve::discoveryDiagnostics() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<ArchiveDiscoveryDiagnostic> diag;
    scanArchivesLocked(&diag);
    return diag;
}

// ---------------------------------------------------------------------------
// 写入
// ---------------------------------------------------------------------------
AppendResult Achieve::appendChecked(const std::string& convKey, const Msg& msg) {
    if (convKey.empty()) return {false, 0, "会话键为空，拒绝写入档案"};

    std::lock_guard<std::mutex> lock(mtx_);
    Conversation& conv = ensureLoadedLocked(convKey);
    if (conv.path.empty()) return {false, 0, "档案路径为空"};

    Msg rec = msg;
    // 显式 ID 只在真正推进时接受（迁移/恢复场景），否则顺延分配
    rec.messageId = msg.messageId > conv.maxId ? msg.messageId : conv.maxId + 1;
    rec.truncated = false;  // 投影标记不落盘

    const std::string line = msgToJson(rec).dump();
    std::ofstream out(conv.path, std::ios::app | std::ios::binary);
    if (!out) return {false, 0, "无法打开档案写入: " + conv.path.string()};
    out << line << '\n';
    out.flush();
    if (!out.good()) return {false, 0, "档案写入/flush 失败: " + conv.path.string()};
    out.close();
    if (out.fail())
        return {false, 0, "档案 close 失败（数据可能未落盘）: " + conv.path.string()};

    // 只有确认落盘成功后才更新缓存与最大 ID
    conv.msgs.push_back(rec);
    conv.lineIds.push_back(rec.messageId);
    conv.maxId = rec.messageId;
    writeIndexLocked(convKey, conv);
    return {true, rec.messageId, ""};
}

AppendResult Achieve::appendChecked(const ConversationKey& key, const Msg& msg) {
    return appendChecked(key.toString(), msg);
}

void Achieve::append(const ConversationKey& key, const Msg& msg) {
    const AppendResult r = appendChecked(key, msg);
    if (!r.ok) {
        // void 接口无法回传失败：日志 + 抛异常，绝不让"写入失败"静默成功
        log::error("Achieve", "档案写入失败: key=" + key.toString() + " " + r.error);
        throw std::runtime_error("档案写入失败: " + r.error);
    }
}

void Achieve::append(const ConversationKey& key, const std::vector<Msg>& msgs) {
    for (const auto& m : msgs) append(key, m);
}

// ---------------------------------------------------------------------------
// 读取（投影）
// ---------------------------------------------------------------------------
std::string Achieve::visibleText(const Msg& m) {
    if (!m.text.empty()) return m.text;
    // 多模态消息 text 可能为空：退化为拼接非 ephemeral 文本 part。
    // 图片 part 是 URL / base64 大载荷，不进入档案投影（冷启动预算会被撑爆）。
    std::string out;
    for (const auto& p : m.parts) {
        if (p.ephemeral || p.kind != Part::Kind::Text || p.text.empty()) continue;
        if (!out.empty()) out += "\n";
        out += p.text;
    }
    return out;
}

ArchiveRecord Achieve::project(const Msg& m, const std::string& convKey) {
    ArchiveRecord r;
    r.conversationKey = convKey;
    r.messageId = m.messageId;
    r.role = m.role;
    r.text = visibleText(m);  // reasoningContent 绝不进入 text
    r.createdAt = m.createdAt;
    r.senderId = m.senderId;
    r.senderName = m.senderName;
    r.platform = m.platform;
    r.groupId = m.groupId;
    r.hasReasoning = !m.reasoningContent.empty();  // 只表示"存在但被排除"
    r.isToolMessage = (m.role == Role::Tool);
    if (m.role == Role::Assistant) {
        for (const auto& tc : m.toolCalls) r.toolNames.push_back(tc.name);  // 只有名字
    }
    r.truncated = m.truncated;
    return r;
}

std::vector<ArchiveRecord> Achieve::dropOrphanToolRecords(
    std::vector<ArchiveRecord> records) {
    // 工具回合完整性：带 toolCalls 的助手锚点必须紧跟同样数量的工具结果才算
    // "闭合"；未闭合的锚点与其后的孤儿 tool 消息整体丢弃 —— 历史工具调用
    // 绝不作为未完成调用重放。
    std::vector<ArchiveRecord> out;
    out.reserve(records.size());
    std::size_t i = 0;
    while (i < records.size()) {
        const ArchiveRecord& r = records[i];
        if (r.isToolMessage) {  // 孤儿 tool 结果：丢弃
            ++i;
            continue;
        }
        if (r.role == Role::Assistant && !r.toolNames.empty()) {
            const std::size_t need = r.toolNames.size();
            bool closed = (i + need) < records.size();
            for (std::size_t k = 1; closed && k <= need; ++k) {
                if (!records[i + k].isToolMessage) closed = false;
            }
            if (!closed) {  // 未闭合的调用回合：锚点也丢弃
                ++i;
                continue;
            }
            out.push_back(r);
            for (std::size_t k = 1; k <= need; ++k) out.push_back(records[i + k]);
            i += need + 1;
            continue;
        }
        out.push_back(r);
        ++i;
    }
    return out;
}

std::vector<Msg> Achieve::load(const ConversationKey& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    return ensureLoadedLocked(key.toString()).msgs;
}

std::size_t Achieve::count(const ConversationKey& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    return ensureLoadedLocked(key.toString()).msgs.size();
}

ColdStartResult Achieve::coldStart(const std::string& convKey,
                                   const ColdStartOptions& opts) {
    std::vector<ArchiveRecord> snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const Conversation& conv = ensureLoadedLocked(convKey);
        const std::size_t window =
            std::max(kColdStartWindowMin,
                     static_cast<std::size_t>(std::max(0, opts.maxMessages)) * 8);
        const std::size_t begin =
            conv.msgs.size() > window ? conv.msgs.size() - window : 0;
        snapshot.reserve(conv.msgs.size() - begin);
        for (std::size_t i = begin; i < conv.msgs.size(); ++i)
            snapshot.push_back(project(conv.msgs[i], convKey));
    }  // 锁已释放：选择逻辑不再触碰档案
    return selectColdStart(snapshot, opts);
}

ColdStartResult Achieve::coldStart(const ConversationKey& key,
                                   const ColdStartOptions& opts) {
    return coldStart(key.toString(), opts);
}

std::int64_t Achieve::latestMessageId(const std::string& convKey) {
    std::lock_guard<std::mutex> lock(mtx_);
    return ensureLoadedLocked(convKey).maxId;
}

std::int64_t Achieve::latestMessageId(const ConversationKey& key) {
    return latestMessageId(key.toString());
}

std::vector<ArchiveRecord> Achieve::readRange(const std::string& convKey,
                                              std::int64_t fromMessageId,
                                              std::int64_t toMessageId,
                                              std::size_t limit) {
    std::vector<ArchiveRecord> records;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const Conversation& conv = ensureLoadedLocked(convKey);
        // conv.msgs 的 messageId 严格递增（文件顺序 = 时间顺序），无需排序
        for (const auto& m : conv.msgs) {
            if (fromMessageId > 0 && m.messageId < fromMessageId) continue;
            if (toMessageId > 0 && m.messageId > toMessageId) continue;
            records.push_back(project(m, convKey));
        }
    }
    records = dropOrphanToolRecords(std::move(records));
    if (limit != 0 && records.size() > limit) records.resize(limit);
    return records;
}

std::vector<ArchiveRecord> Achieve::readRange(const ConversationKey& key,
                                              std::int64_t fromMessageId,
                                              std::int64_t toMessageId,
                                              std::size_t limit) {
    return readRange(key.toString(), fromMessageId, toMessageId, limit);
}

std::vector<ArchiveRecord> Achieve::readRecent(const std::string& convKey,
                                               std::size_t limit) {
    std::vector<ArchiveRecord> records;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const Conversation& conv = ensureLoadedLocked(convKey);
        const std::size_t n =
            (limit == 0 || limit > conv.msgs.size()) ? conv.msgs.size() : limit;
        records.reserve(n);
        for (std::size_t i = conv.msgs.size() - n; i < conv.msgs.size(); ++i)
            records.push_back(project(conv.msgs[i], convKey));  // 时间正序
    }
    return dropOrphanToolRecords(std::move(records));
}

std::vector<ArchiveRecord> Achieve::readRecent(const ConversationKey& key,
                                               std::size_t limit) {
    return readRecent(key.toString(), limit);
}

std::vector<ArchiveRecord> Achieve::search(const std::string& convKey,
                                           const std::string& query,
                                           std::size_t limit) {
    std::vector<ArchiveRecord> out;
    if (query.empty()) return out;
    const std::string needle = toLowerAscii(query);

    std::lock_guard<std::mutex> lock(mtx_);
    const Conversation& conv = ensureLoadedLocked(convKey);
    for (const auto& m : conv.msgs) {
        // 只检索用户/助手可见正文：不返回工具消息、reasoning、工具参数
        if (m.role != Role::User && m.role != Role::Assistant) continue;
        ArchiveRecord r = project(m, convKey);
        if (r.text.empty()) continue;
        if (toLowerAscii(r.text).find(needle) == std::string::npos) continue;
        out.push_back(std::move(r));
        if (limit != 0 && out.size() >= limit) break;
    }
    return out;
}

std::vector<ArchiveRecord> Achieve::search(const ConversationKey& key,
                                           const std::string& query,
                                           std::size_t limit) {
    return search(key.toString(), query, limit);
}

std::vector<ArchiveDamage> Achieve::damageReport() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return damage_;
}

// ---------------------------------------------------------------------------
// F5：未总结范围
// ---------------------------------------------------------------------------
Achieve::PendingRange Achieve::pendingRange(const std::string& convKey,
                                            std::int64_t afterMessageId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const Conversation& conv = ensureLoadedLocked(convKey);
    PendingRange out;
    for (const auto& m : conv.msgs) {
        if (m.messageId <= 0 || m.messageId <= afterMessageId) continue;
        // 只统计用户 + 可见助手：不计 reasoning、工具消息、系统消息、
        // 框架提醒和旧摘要标记
        if (m.role != Role::User && m.role != Role::Assistant) continue;
        if (m.isSummary) continue;
        const std::string text = visibleText(m);
        if (isFrameworkReminder(text)) continue;
        if (m.role == Role::Assistant && text.empty()) continue;  // 只有 toolCalls
        if (out.fromMessageId == 0) out.fromMessageId = m.messageId;
        // to 与 from/count 同口径：范围内最后一条【可总结】消息的 ID。
        // 末尾只有工具/纯 reasoning 消息时不会指向它们（调度器 A 的语义）。
        out.toMessageId = m.messageId;
        ++out.count;
    }
    return out;  // 没有候选 → {0, 0, 0}
}

Achieve::PendingRange Achieve::pendingRange(const ConversationKey& key,
                                            std::int64_t afterMessageId) const {
    return pendingRange(key.toString(), afterMessageId);
}

} // namespace mio
