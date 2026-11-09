-- plugins/lua/dreaming.lua
-- 「做梦」插件：每天凌晨 3 点（03:00-03:59 窗口内首次检查时）投递一条 InternalPulse
-- 事件唤醒模型，让模型回顾当天、提炼值得长期记住的内容并写入记忆，然后保持安静。
--
-- 依赖（当前引擎已提供）：
--   * mio.local_time() / mio.now_ms()：本地时间与 epoch 毫秒
--   * mio.post_event()：异步投递入站事件（独立线程在沙箱锁外投递，不会重入死锁）
--
-- 写入长期记忆依赖 config.json 的 mcp 段挂载记忆类 MCP Server（README 提到的
-- graphiti 等）。若没有记忆工具，脚本会提示模型退化为用 set_notes 写联系人备注。
--
-- 可调参数直接改下面的常量；重载：/plugin reload lua:dreaming

local DREAM_HOUR = 3          -- 触发窗口的小时（0-23）
local CHECK_INTERVAL = 60     -- 轮询间隔（秒）
local MEMORY_TOOL_HINT = "长期记忆写入工具（例如 graphiti_add_memory）"

local firedDay = nil          -- 已触发过的日期 "YYYY-MM-DD"（仅内存态，进程重启会重置）

local function buildDreamEvent(nowStr, nowMs)
    local day = nowStr:sub(1, 10)
    local content = string.format([[
现在是你独处的深夜（%s，每日整理时间）。请做一次「做梦」整理，不要打扰任何人：

1. 如果今天没有任何事件，跳过总结，直接调用 end_turn 结束本回合。
2. 提炼值得长期记住的内容：新认识的人、重要的约定与承诺、对方明确表达过的喜好/雷点、今天让你印象深刻的情绪与想法。
3. 把这些内容写入长期记忆：优先调用你被提供的 %s；若当前没有可用的记忆写入工具，就把要点压缩成 1-2 句，用 set_notes 记到最相关的人名下（只写通讯录里已知的人；没有合适对象就跳过）。
4. 全程不要向任何会话发送消息；整理完成后调用 end_turn 结束本回合。
]], nowStr, day, day, MEMORY_TOOL_HINT)

    return {
        category = "InternalPulse",
        tier = "DirectFull",
        source = "lua:dreaming",
        title = "夜间整理",
        channelId = "",           -- 不绑定任何会话：本轮不会外发消息
        actorId = "",
        content = content,
        timestamp = nowMs,
    }
end

mio.every(CHECK_INTERVAL, function()
    local nowStr = mio.local_time()                  -- "YYYY-MM-DD HH:MM:SS"
    local day = nowStr:sub(1, 10)
    local hour = tonumber(nowStr:sub(12, 13))
    if hour ~= DREAM_HOUR or firedDay == day then
        return
    end

    firedDay = day
    if mio.post_event(buildDreamEvent(nowStr, mio.now_ms())) then
        mio.log.info("dreaming: 已投递夜间整理事件 " .. nowStr)
    else
        mio.log.warn("dreaming: 夜间整理事件入队失败，下一次检查重试 " .. nowStr)
        firedDay = nil
    end
end)

return {
    name = "dreaming",
    version = "0.1.0"
}
