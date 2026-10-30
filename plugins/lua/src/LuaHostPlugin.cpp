#include "mio/plugin.hpp"

#include <lua.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace mio {
namespace {

constexpr const char* kTag = "LuaHost";
constexpr std::size_t kDefaultMaxMemoryBytes = 8 * 1024 * 1024; // 8 MB
constexpr int kDefaultInstructionQuota = 500'000;

// ---- JSON <-> Lua 转换 ------------------------------------------------------
void pushJson(lua_State* L, const nlohmann::json& j) {
    if (j.is_null()) {
        lua_pushnil(L);
    } else if (j.is_boolean()) {
        lua_pushboolean(L, j.get<bool>() ? 1 : 0);
    } else if (j.is_number_integer()) {
        lua_pushinteger(L, static_cast<lua_Integer>(j.get<std::int64_t>()));
    } else if (j.is_number_unsigned()) {
        lua_pushinteger(L, static_cast<lua_Integer>(j.get<std::uint64_t>()));
    } else if (j.is_number_float()) {
        lua_pushnumber(L, static_cast<lua_Number>(j.get<double>()));
    } else if (j.is_string()) {
        const auto& s = j.get_ref<const std::string&>();
        lua_pushlstring(L, s.data(), s.size());
    } else if (j.is_array()) {
        lua_createtable(L, static_cast<int>(j.size()), 0);
        for (std::size_t i = 0; i < j.size(); ++i) {
            pushJson(L, j[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
    } else if (j.is_object()) {
        lua_createtable(L, 0, static_cast<int>(j.size()));
        for (auto it = j.begin(); it != j.end(); ++it) {
            lua_pushlstring(L, it.key().data(), it.key().size());
            pushJson(L, it.value());
            lua_rawset(L, -3);
        }
    } else {
        lua_pushnil(L);
    }
}

nlohmann::json toJson(lua_State* L, int idx, int depth = 0) {
    if (depth > 32) return nullptr;
    const int absIdx = lua_absindex(L, idx);
    const int t = lua_type(L, absIdx);
    switch (t) {
    case LUA_TNIL:
    case LUA_TNONE:
        return nullptr;
    case LUA_TBOOLEAN:
        return lua_toboolean(L, absIdx) != 0;
    case LUA_TNUMBER:
        if (lua_isinteger(L, absIdx)) {
            return static_cast<std::int64_t>(lua_tointeger(L, absIdx));
        }
        return static_cast<double>(lua_tonumber(L, absIdx));
    case LUA_TSTRING: {
        std::size_t len = 0;
        const char* s = lua_tolstring(L, absIdx, &len);
        return std::string(s ? s : "", len);
    }
    case LUA_TTABLE: {
        // 判断是否为连续整数下标数组 (1..N)
        bool isArray = true;
        lua_Integer maxIndex = 0;
        std::size_t count = 0;
        lua_pushnil(L);
        while (lua_next(L, absIdx) != 0) {
            ++count;
            if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
                lua_Integer k = lua_tointeger(L, -2);
                if (k >= 1) {
                    if (k > maxIndex) maxIndex = k;
                } else {
                    isArray = false;
                }
            } else {
                isArray = false;
            }
            lua_pop(L, 1);
        }
        if (count == 0) {
            return nlohmann::json::object();
        }
        if (isArray && static_cast<std::size_t>(maxIndex) == count) {
            nlohmann::json arr = nlohmann::json::array();
            for (lua_Integer i = 1; i <= maxIndex; ++i) {
                lua_geti(L, absIdx, i);
                arr.push_back(toJson(L, -1, depth + 1));
                lua_pop(L, 1);
            }
            return arr;
        }
        nlohmann::json obj = nlohmann::json::object();
        lua_pushnil(L);
        while (lua_next(L, absIdx) != 0) {
            std::string key;
            if (lua_type(L, -2) == LUA_TSTRING) {
                std::size_t klen = 0;
                const char* ks = lua_tolstring(L, -2, &klen);
                key.assign(ks ? ks : "", klen);
            } else if (lua_type(L, -2) == LUA_TNUMBER) {
                if (lua_isinteger(L, -2)) {
                    key = std::to_string(lua_tointeger(L, -2));
                } else {
                    key = std::to_string(lua_tonumber(L, -2));
                }
            }
            if (!key.empty()) {
                obj[key] = toJson(L, -1, depth + 1);
            }
            lua_pop(L, 1);
        }
        return obj;
    }
    default:
        return nullptr;
    }
}

// ---- 单脚本沙箱实例 --------------------------------------------------------
class LuaScriptSandbox {
public:
    struct TimerEntry {
        int intervalSec = 60;
        int callbackRef = LUA_NOREF;
        std::chrono::steady_clock::time_point nextRun;
    };

    LuaScriptSandbox(std::string scriptId,
                     std::filesystem::path path,
                     sdk::Host& host,
                     std::size_t maxMemoryBytes = kDefaultMaxMemoryBytes)
        : scriptId_(std::move(scriptId)),
          path_(std::move(path)),
          host_(host),
          maxMemoryBytes_(maxMemoryBytes) {}

    ~LuaScriptSandbox() {
        stop();
    }

    const std::string& id() const { return scriptId_; }
    const std::filesystem::path& path() const { return path_; }
    std::size_t usedMemoryBytes() const { return usedMemory_.load(); }
    const std::vector<std::string>& registeredTools() const { return registeredTools_; }

    // 执行当前脚本注册的 on_event / on_outbound 钩子
    std::optional<nlohmann::json> runHookRefs(bool isOutbound, const nlohmann::json& payload, bool& drop) {
        std::lock_guard<std::mutex> lock(vmMtx_);
        if (!L_) return std::nullopt;
        const auto& refs = isOutbound ? onOutboundRefs_ : onEventRefs_;
        if (refs.empty()) return std::nullopt;

        nlohmann::json current = payload;
        bool modified = false;
        for (int ref : refs) {
            resetInstructionHook(L_);
            lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
            pushJson(L_, current);
            if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
                const char* err = lua_tostring(L_, -1);
                host_.warn(kTag, "Lua Hook 异常 [" + scriptId_ + "]: " + (err ? err : "unknown"));
                lua_pop(L_, 1);
                continue;
            }
            if (lua_isboolean(L_, -1) && !lua_toboolean(L_, -1)) {
                drop = true;
                lua_pop(L_, 1);
                return std::nullopt;
            }
            if (lua_istable(L_, -1)) {
                current = toJson(L_, -1);
                modified = true;
            }
            lua_pop(L_, 1);
        }
        return modified ? std::optional<nlohmann::json>(std::move(current)) : std::nullopt;
    }

    bool loadAndStart() {
        std::lock_guard<std::mutex> lock(vmMtx_);
        L_ = lua_newstate(&LuaScriptSandbox::customAlloc, this);
        if (!L_) {
            host_.error(kTag, "无法创建 Lua 状态机: " + scriptId_);
            return false;
        }

        openSafeLibs(L_);
        bindMioApi(L_);

        resetInstructionHook(L_);
        if (luaL_loadfile(L_, path_.string().c_str()) != LUA_OK) {
            const char* err = lua_tostring(L_, -1);
            host_.error(kTag, "加载 Lua 脚本失败 [" + scriptId_ + "]: " + (err ? err : "unknown"));
            lua_pop(L_, 1);
            lua_close(L_);
            L_ = nullptr;
            return false;
        }

        if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
            const char* err = lua_tostring(L_, -1);
            host_.error(kTag, "执行 Lua 脚本失败 [" + scriptId_ + "]: " + (err ? err : "unknown"));
            lua_pop(L_, 1);
            lua_close(L_);
            L_ = nullptr;
            return false;
        }

        if (lua_istable(L_, -1)) {
            lua_getfield(L_, -1, "name");
            if (lua_isstring(L_, -1)) metaName_ = lua_tostring(L_, -1);
            lua_pop(L_, 1);

            lua_getfield(L_, -1, "version");
            if (lua_isstring(L_, -1)) metaVersion_ = lua_tostring(L_, -1);
            lua_pop(L_, 1);
        }
        lua_pop(L_, 1);

        if (!timers_.empty()) {
            running_.store(true);
            timerThread_ = std::thread(&LuaScriptSandbox::timerLoop, this);
        }

        host_.info(kTag, "已加载 Lua 插件: " + scriptId_ +
                             (metaVersion_.empty() ? "" : (" v" + metaVersion_)) +
                             " (tools=" + std::to_string(registeredTools_.size()) + ")");
        return true;
    }

    void stop() {
        if (running_.exchange(false)) {
            timerCv_.notify_all();
            if (timerThread_.joinable()) {
                timerThread_.join();
            }
        }
        std::lock_guard<std::mutex> lock(vmMtx_);
        for (const auto& toolName : registeredTools_) {
            host_.unregisterTool(toolName);
        }
        registeredTools_.clear();
        if (L_) {
            lua_close(L_);
            L_ = nullptr;
        }
    }

private:
    static void* customAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) noexcept {
        auto* self = static_cast<LuaScriptSandbox*>(ud);
        if (nsize == 0) {
            if (ptr) {
                self->usedMemory_.fetch_sub(osize);
                std::free(ptr);
            }
            return nullptr;
        }
        const std::size_t oldBytes = ptr ? osize : 0;
        const std::size_t cur = self->usedMemory_.load();
        if (nsize > oldBytes && (cur + (nsize - oldBytes)) > self->maxMemoryBytes_) {
            return nullptr; // 触发 Lua OOM
        }
        void* newPtr = std::realloc(ptr, nsize);
        if (newPtr) {
            if (nsize >= oldBytes) {
                self->usedMemory_.fetch_add(nsize - oldBytes);
            } else {
                self->usedMemory_.fetch_sub(oldBytes - nsize);
            }
        }
        return newPtr;
    }

    static void instructionHook(lua_State* L, lua_Debug* /*ar*/) {
        luaL_error(L, "Lua script exceeded instruction quota (%d)", kDefaultInstructionQuota);
    }

    static void resetInstructionHook(lua_State* L) {
        lua_sethook(L, &LuaScriptSandbox::instructionHook, LUA_MASKCOUNT, kDefaultInstructionQuota);
    }

    static void openSafeLibs(lua_State* L) {
        luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(L, 1);

        // 移除 base 中的文件系统加载函数
        lua_pushnil(L);
        lua_setglobal(L, "dofile");
        lua_pushnil(L);
        lua_setglobal(L, "loadfile");
    }

    void bindMioApi(lua_State* L) {
        lua_newtable(L); // mio

        // mio.log table & callable
        lua_newtable(L);
        auto pushLogFn = [&](const char* name, mio_log_level lv) {
            lua_pushlightuserdata(L, this);
            lua_pushinteger(L, static_cast<int>(lv));
            lua_pushcclosure(L, &LuaScriptSandbox::luaLogLevel, 2);
            lua_setfield(L, -2, name);
        };
        pushLogFn("debug", MIO_LOG_DEBUG);
        pushLogFn("info", MIO_LOG_INFO);
        pushLogFn("warn", MIO_LOG_WARN);
        pushLogFn("error", MIO_LOG_ERROR);
        lua_setfield(L, -2, "log");

        // mio.register_tool(def, fn)
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, &LuaScriptSandbox::luaRegisterTool, 1);
        lua_setfield(L, -2, "register_tool");

        // mio.post_event(ev)
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, &LuaScriptSandbox::luaPostEvent, 1);
        lua_setfield(L, -2, "post_event");

        // mio.invoke_tool(name, args)
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, &LuaScriptSandbox::luaInvokeTool, 1);
        lua_setfield(L, -2, "invoke_tool");

        // mio.on_event(fn)
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, &LuaScriptSandbox::luaOnEvent, 1);
        lua_setfield(L, -2, "on_event");

        // mio.on_outbound(fn)
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, &LuaScriptSandbox::luaOnOutbound, 1);
        lua_setfield(L, -2, "on_outbound");

        // mio.every(seconds, fn)
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, &LuaScriptSandbox::luaEvery, 1);
        lua_setfield(L, -2, "every");

        // mio.json.{encode, decode}
        lua_newtable(L);
        lua_pushcfunction(L, &LuaScriptSandbox::luaJsonEncode);
        lua_setfield(L, -2, "encode");
        lua_pushcfunction(L, &LuaScriptSandbox::luaJsonDecode);
        lua_setfield(L, -2, "decode");
        lua_setfield(L, -2, "json");

        lua_setglobal(L, "mio");
    }

    static int luaLogLevel(lua_State* L) {
        auto* self = static_cast<LuaScriptSandbox*>(lua_touserdata(L, lua_upvalueindex(1)));
        const auto lv = static_cast<mio_log_level>(lua_tointeger(L, lua_upvalueindex(2)));
        const char* msg = luaL_checkstring(L, 1);
        if (self) {
            self->host_.log(lv, "lua:" + self->scriptId_, msg ? msg : "");
        }
        return 0;
    }

    static int luaRegisterTool(lua_State* L) {
        auto* self = static_cast<LuaScriptSandbox*>(lua_touserdata(L, lua_upvalueindex(1)));
        luaL_checktype(L, 1, LUA_TTABLE);
        luaL_checktype(L, 2, LUA_TFUNCTION);

        nlohmann::json def = toJson(L, 1);
        const std::string toolName = def.value("name", "");
        if (toolName.empty()) {
            return luaL_error(L, "mio.register_tool: def.name is required");
        }
        def["owner"] = "lua:" + self->scriptId_;

        lua_pushvalue(L, 2);
        const int fnRef = luaL_ref(L, LUA_REGISTRYINDEX);

        const bool ok = self->host_.registerTool(
            def,
            [self, fnRef, toolName](const nlohmann::json& args) -> std::string {
                return self->invokeLuaTool(fnRef, toolName, args);
            });
        if (ok) {
            self->registeredTools_.push_back(toolName);
        } else {
            luaL_unref(L, LUA_REGISTRYINDEX, fnRef);
        }
        lua_pushboolean(L, ok ? 1 : 0);
        return 1;
    }

    static int luaPostEvent(lua_State* L) {
        auto* self = static_cast<LuaScriptSandbox*>(lua_touserdata(L, lua_upvalueindex(1)));
        luaL_checktype(L, 1, LUA_TTABLE);
        nlohmann::json ev = toJson(L, 1);
        if (!ev.contains("source") || !ev["source"].is_string() || ev["source"].get<std::string>().empty()) {
            ev["source"] = "lua:" + self->scriptId_;
        }
        const bool ok = self->host_.postEvent(ev);
        lua_pushboolean(L, ok ? 1 : 0);
        return 1;
    }

    static int luaInvokeTool(lua_State* L) {
        auto* self = static_cast<LuaScriptSandbox*>(lua_touserdata(L, lua_upvalueindex(1)));
        const char* name = luaL_checkstring(L, 1);
        nlohmann::json args = lua_istable(L, 2) ? toJson(L, 2) : nlohmann::json::object();
        const std::string res = self->host_.invokeTool(name ? name : "", args);
        lua_pushlstring(L, res.data(), res.size());
        return 1;
    }

    static int luaOnEvent(lua_State* L) {
        auto* self = static_cast<LuaScriptSandbox*>(lua_touserdata(L, lua_upvalueindex(1)));
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        self->onEventRefs_.push_back(ref);
        return 0;
    }

    static int luaOnOutbound(lua_State* L) {
        auto* self = static_cast<LuaScriptSandbox*>(lua_touserdata(L, lua_upvalueindex(1)));
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        self->onOutboundRefs_.push_back(ref);
        return 0;
    }

    static int luaEvery(lua_State* L) {
        auto* self = static_cast<LuaScriptSandbox*>(lua_touserdata(L, lua_upvalueindex(1)));
        const int sec = std::max(1, static_cast<int>(luaL_checkinteger(L, 1)));
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_pushvalue(L, 2);
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        TimerEntry t;
        t.intervalSec = sec;
        t.callbackRef = ref;
        t.nextRun = std::chrono::steady_clock::now() + std::chrono::seconds(sec);
        self->timers_.push_back(t);
        return 0;
    }

    static int luaJsonEncode(lua_State* L) {
        nlohmann::json j = toJson(L, 1);
        const std::string s = j.dump();
        lua_pushlstring(L, s.data(), s.size());
        return 1;
    }

    static int luaJsonDecode(lua_State* L) {
        std::size_t len = 0;
        const char* s = luaL_checklstring(L, 1, &len);
        try {
            nlohmann::json j = nlohmann::json::parse(s, s + len);
            pushJson(L, j);
            return 1;
        } catch (const std::exception& e) {
            return luaL_error(L, "mio.json.decode error: %s", e.what());
        }
    }

    std::string invokeLuaTool(int fnRef, const std::string& toolName, const nlohmann::json& args) {
        std::lock_guard<std::mutex> lock(vmMtx_);
        if (!L_) return "error: Lua VM 已关闭";

        resetInstructionHook(L_);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, fnRef);
        pushJson(L_, args);

        if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
            const char* err = lua_tostring(L_, -1);
            std::string errMsg = std::string("error: Lua tool [") + toolName + "] failed: " +
                                 (err ? err : "unknown");
            lua_pop(L_, 1);
            host_.warn(kTag, errMsg);
            return errMsg;
        }

        std::string result;
        if (lua_isstring(L_, -1)) {
            std::size_t len = 0;
            const char* s = lua_tolstring(L_, -1, &len);
            result.assign(s ? s : "", len);
        } else if (!lua_isnil(L_, -1)) {
            result = toJson(L_, -1).dump();
        }
        lua_pop(L_, 1);
        return result;
    }

    void timerLoop() {
        while (running_.load()) {
            const auto now = std::chrono::steady_clock::now();
            auto nextWake = now + std::chrono::seconds(5);
            {
                std::lock_guard<std::mutex> lock(vmMtx_);
                if (L_) {
                    for (auto& t : timers_) {
                        if (now >= t.nextRun) {
                            resetInstructionHook(L_);
                            lua_rawgeti(L_, LUA_REGISTRYINDEX, t.callbackRef);
                            if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
                                const char* err = lua_tostring(L_, -1);
                                host_.warn(kTag, "Lua 定时器异常 [" + scriptId_ + "]: " +
                                                     (err ? err : "unknown"));
                                lua_pop(L_, 1);
                            }
                            t.nextRun = std::chrono::steady_clock::now() +
                                        std::chrono::seconds(t.intervalSec);
                        }
                        if (t.nextRun < nextWake) {
                            nextWake = t.nextRun;
                        }
                    }
                }
            }
            std::unique_lock<std::mutex> wlock(timerMtx_);
            timerCv_.wait_until(wlock, nextWake, [this] { return !running_.load(); });
        }
    }

    std::string scriptId_;
    std::filesystem::path path_;
    sdk::Host& host_;
    std::size_t maxMemoryBytes_;
    std::atomic<std::size_t> usedMemory_{0};

    std::mutex vmMtx_;
    lua_State* L_ = nullptr;
    std::string metaName_;
    std::string metaVersion_;
    std::vector<std::string> registeredTools_;
    std::vector<int> onEventRefs_;
    std::vector<int> onOutboundRefs_;
    std::vector<TimerEntry> timers_;

    std::atomic<bool> running_{false};
    std::mutex timerMtx_;
    std::condition_variable timerCv_;
    std::thread timerThread_;
};

// ---- LuaHost 原生插件（管理 plugins/lua/*.lua）-----------------------------
class LuaHostPlugin final : public sdk::Plugin {
public:
    LuaHostPlugin(sdk::Host& host, const nlohmann::json& cfg)
        : host_(host) {
        if (cfg.is_object() && cfg.contains("dir") && cfg["dir"].is_string()) {
            scriptsDir_ = cfg["dir"].get<std::string>();
        } else {
            scriptsDir_ = "plugins/lua";
        }

        host_.registerHook("on_event", 100,
                           [this](const nlohmann::json& payload, bool& drop) {
                               return dispatchHooks(false, payload, drop);
                           });
        host_.registerHook("on_outbound", 100,
                           [this](const nlohmann::json& payload, bool& drop) {
                               return dispatchHooks(true, payload, drop);
                           });
    }

    ~LuaHostPlugin() override {
        stop();
    }

    bool start() override {
        std::lock_guard<std::mutex> lock(mtx_);
        loadScriptsInDirLocked();
        return true;
    }

    void stop() override {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [_, sandbox] : scripts_) {
            if (sandbox) sandbox->stop();
        }
        scripts_.clear();
    }

    bool reconfigure(const nlohmann::json& cfg) override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (cfg.is_object() && cfg.contains("dir") && cfg["dir"].is_string()) {
            scriptsDir_ = cfg["dir"].get<std::string>();
        }
        if (cfg.is_object() && cfg.contains("reload_script") && cfg["reload_script"].is_string()) {
            const std::string targetId = cfg["reload_script"].get<std::string>();
            if (!targetId.empty()) {
                auto it = scripts_.find(targetId);
                if (it != scripts_.end() && it->second) {
                    it->second->stop();
                    scripts_.erase(it);
                }
                const auto file = scriptsDir_ / (targetId + ".lua");
                if (std::filesystem::exists(file)) {
                    auto sandbox = std::make_unique<LuaScriptSandbox>(targetId, file, host_);
                    if (sandbox->loadAndStart()) {
                        scripts_[targetId] = std::move(sandbox);
                        return true;
                    }
                }
                return false;
            }
        }
        for (auto& [_, sandbox] : scripts_) {
            if (sandbox) sandbox->stop();
        }
        scripts_.clear();
        loadScriptsInDirLocked();
        return true;
    }

private:
    std::optional<nlohmann::json> dispatchHooks(bool isOutbound,
                                                const nlohmann::json& payload,
                                                bool& drop) {
        std::lock_guard<std::mutex> lock(mtx_);
        nlohmann::json current = payload;
        bool anyModified = false;
        for (auto& [_, sandbox] : scripts_) {
            if (!sandbox) continue;
            auto res = sandbox->runHookRefs(isOutbound, current, drop);
            if (drop) return std::nullopt;
            if (res.has_value()) {
                current = std::move(*res);
                anyModified = true;
            }
        }
        return anyModified ? std::optional<nlohmann::json>(std::move(current)) : std::nullopt;
    }
    void loadScriptsInDirLocked() {
        std::error_code ec;
        if (!std::filesystem::exists(scriptsDir_, ec) ||
            !std::filesystem::is_directory(scriptsDir_, ec)) {
            host_.info(kTag, "Lua 插件目录不存在或为空: " + scriptsDir_.string());
            return;
        }

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(scriptsDir_, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".lua") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());

        for (const auto& file : files) {
            const std::string id = file.stem().string();
            auto sandbox = std::make_unique<LuaScriptSandbox>(id, file, host_);
            if (sandbox->loadAndStart()) {
                scripts_[id] = std::move(sandbox);
            }
        }
    }

    sdk::Host& host_;
    std::filesystem::path scriptsDir_;
    std::map<std::string, std::unique_ptr<LuaScriptSandbox>> scripts_;
    std::mutex mtx_;
};

} // namespace
} // namespace mio

MIO_PLUGIN(mio::LuaHostPlugin, "lua-host", "0.1.0")
