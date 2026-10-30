-- plugins/lua/dice.lua
-- 示例 Lua 轻量插件：注册 roll_dice 工具

mio.register_tool({
    name = "roll_dice",
    description = "掷骰子（支持 NdM 格式，如 2d6、1d20、3d100）。",
    parameters = {
        type = "object",
        properties = {
            expr = {
                type = "string",
                description = "骰子表达式，如 '2d6' 或 '1d20'"
            }
        },
        required = { "expr" }
    }
}, function(args)
    local expr = tostring(args.expr or "1d6")
    local n_str, m_str = expr:match("^(%d+)[dD](%d+)$")
    local n = tonumber(n_str) or 1
    local m = tonumber(m_str) or 6
    if n < 1 then n = 1 end
    if n > 100 then n = 100 end
    if m < 2 then m = 2 end
    if m > 10000 then m = 10000 end

    local rolls = {}
    local sum = 0
    for i = 1, n do
        local r = math.random(1, m)
        rolls[i] = tostring(r)
        sum = sum + r
    end
    return string.format("%dd%d = [%s] (合计: %d)", n, m, table.concat(rolls, ", "), sum)
end)

return {
    name = "dice",
    version = "0.1.0"
}
