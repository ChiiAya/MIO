#include "adapters/napcat/MediaResolver.h"

#include <cpr/cpr.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "log/Log.h"
#include "providers/asr/AsrProvider.h"

namespace mio {

namespace {

constexpr const char* kTag = "Media";

bool isRegularFile(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// 返回带点的小写后缀（".jpg"）；非法/过长返回空串
std::string extensionOf(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return "";
    if (name.size() - dot > 6) return "";
    std::string ext = toLower(name.substr(dot));
    for (char c : ext) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.')) return "";
    }
    return ext;
}

std::string sanitizeStem(const std::string& s) {
    std::string out;
    out.reserve(std::min<std::size_t>(s.size(), 32));
    for (char c : s) {
        if (out.size() >= 32) break;
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            out.push_back(c);
        }
    }
    return out;
}

std::string mimeFromName(const std::string& name) {
    const std::string ext = extensionOf(name);
    if (ext == ".png") return "image/png";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".bmp") return "image/bmp";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    return "image/jpeg";  // 视觉端多数支持 jpeg；未知后缀按 jpeg 兜底
}

std::string base64Encode(const std::vector<char>& data) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const std::uint32_t n =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + 2]));
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back(kTable[n & 0x3F]);
        i += 3;
    }

    const std::size_t rest = data.size() - i;
    if (rest == 1) {
        const std::uint32_t n =
            static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << 16;
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        const std::uint32_t n =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool readFileBytes(const std::string& path, std::size_t maxBytes,
                   std::vector<char>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0 || static_cast<std::size_t>(size) > maxBytes) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(in) || in.eof();
}

bool writeFileBytes(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

std::string uniqueCacheName(const MediaAttachment& a) {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    static std::atomic<unsigned> seq{0};
    const std::string base = !a.fileName.empty() ? a.fileName : a.url;
    std::string stem = sanitizeStem(!a.fileId.empty() ? a.fileId : base);
    std::string name = std::to_string(now) + "_" +
                       std::to_string(seq.fetch_add(1));
    if (!stem.empty()) name += "_" + stem;
    name += extensionOf(base);
    return name;
}

const char* kindName(MediaKind kind) {
    switch (kind) {
    case MediaKind::Image: return "图片";
    case MediaKind::Audio: return "语音";
    case MediaKind::Video: return "视频";
    case MediaKind::File: return "文件";
    }
    return "媒体";
}

// 在消息正文后追加提示：保持 [图片]/[语音] 标记在最前，便于模型理解上下文
void appendText(std::string& text, const std::string& note) {
    if (note.empty()) return;
    if (!text.empty()) text += " ";
    text += note;
}

} // namespace

MediaResolver::MediaResolver(MediaConfig cfg, std::shared_ptr<AsrProvider> asr)
    : cfg_(std::move(cfg)), asr_(std::move(asr)) {}

bool MediaResolver::kindEnabled(MediaKind kind) const {
    switch (kind) {
    case MediaKind::Image: return cfg_.images;
    case MediaKind::Audio: return cfg_.audio;
    case MediaKind::Video: return cfg_.video;
    case MediaKind::File: return cfg_.files;
    }
    return false;
}

std::string MediaResolver::download(const MediaAttachment& attachment) const {
    if (attachment.url.empty()) return "";

    const std::string fileName = uniqueCacheName(attachment);
    std::error_code ec;
    std::filesystem::create_directories(cfg_.cacheDir, ec);
    if (ec) {
        log::warn(kTag, "创建媒体缓存目录失败: " + cfg_.cacheDir + " (" +
                            ec.message() + ")");
        return "";
    }
    const std::string path =
        (std::filesystem::path(cfg_.cacheDir) / fileName).string();

    const cpr::Response r = cpr::Get(
        cpr::Url{attachment.url},
        cpr::Header{{"User-Agent", "MIO/1.0"}},
        cpr::Timeout{std::chrono::milliseconds(cfg_.downloadTimeoutMs)},
        cpr::ConnectTimeout{std::chrono::milliseconds(10'000)},
        cpr::Redirect{true});

    if (r.error.code != cpr::ErrorCode::OK) {
        log::warn(kTag, std::string("媒体下载失败(传输层): ") + r.error.message);
        return "";
    }
    if (r.status_code < 200 || r.status_code >= 300) {
        log::warn(kTag, "媒体下载失败(HTTP " + std::to_string(r.status_code) +
                            "): " + attachment.url);
        return "";
    }
    if (r.text.size() > cfg_.maxFileBytes) {
        log::warn(kTag, "媒体超过大小上限，已跳过: " +
                            std::to_string(r.text.size()) + " bytes");
        return "";
    }
    if (!writeFileBytes(path, r.text)) {
        log::warn(kTag, "媒体写入缓存失败: " + path);
        return "";
    }
    return path;
}

void MediaResolver::resolve(IncomingMessage& msg) const {
    if (!cfg_.enabled || msg.attachments.empty()) return;

    std::vector<Part> imageParts;

    for (const auto& attachment : msg.attachments) {
        if (!kindEnabled(attachment.kind)) continue;

        // 1) 同机部署时 NapCat 直接给绝对路径，省掉一次下载
        std::string path = attachment.localPath;
        if (!isRegularFile(path)) {
            path = download(attachment);
        }
        if (!isRegularFile(path)) {
            appendText(msg.text,
                       std::string("[") + kindName(attachment.kind) +
                           "不可用：无本地路径且下载失败]");
            log::warn(kTag, std::string("附件无法落地: kind=") +
                                kindName(attachment.kind) +
                                " url=" + attachment.url +
                                " fileId=" + attachment.fileId);
            continue;
        }

        switch (attachment.kind) {
        case MediaKind::Image: {
            std::vector<char> bytes;
            if (!readFileBytes(path, cfg_.maxFileBytes, bytes)) {
                appendText(msg.text, "[图片读取失败或超过大小上限]");
                log::warn(kTag, "图片读取失败: " + path);
                break;
            }
            const std::string mime = attachment.mimeType.empty()
                                         ? mimeFromName(!attachment.fileName.empty()
                                                            ? attachment.fileName
                                                            : path)
                                         : attachment.mimeType;
            Part part;
            part.kind = Part::Kind::Image;
            part.text = "data:" + mime + ";base64," + base64Encode(bytes);
            // 默认不落档案：base64 会让 JSONL 体积膨胀若干倍
            part.ephemeral = !cfg_.persistImages;
            imageParts.push_back(std::move(part));
            break;
        }
        case MediaKind::Audio: {
            if (!asr_) {
                appendText(msg.text, "[语音已缓存: " + path +
                                         "（未启用语音识别）]");
                break;
            }
            try {
                const std::string text = asr_->transcribeFile(path);
                if (text.empty()) {
                    appendText(msg.text, "[语音转写为空]");
                } else {
                    appendText(msg.text, "[语音转写] " + text);
                }
            } catch (const std::exception& e) {
                appendText(msg.text, std::string("[语音转写失败: ") + e.what() +
                                         "；音频已缓存: " + path + "]");
                log::warn(kTag, std::string("语音转写失败: ") + e.what());
            }
            break;
        }
        case MediaKind::Video:
            appendText(msg.text, "[视频已缓存: " + path + "]");
            break;
        case MediaKind::File:
            appendText(msg.text, "[文件已缓存: " + path + "]");
            break;
        }
    }

    if (!imageParts.empty()) {
        std::vector<Part> parts;
        parts.reserve(imageParts.size());
        for (auto& part : imageParts) parts.push_back(std::move(part));
        msg.parts = std::move(parts);
    }
}

} // namespace mio
