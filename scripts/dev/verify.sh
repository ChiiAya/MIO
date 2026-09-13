#!/usr/bin/env bash
# MIO 记忆系统改造 —— 一键验证脚本（主 agent 维护）
#
#   scripts/dev/verify.sh          构建主程序 + 测试，跑全部用例
#   scripts/dev/verify.sh <过滤词>  只跑匹配的用例
#
# 不联网、不使用真实用户数据；用 flock 串行化构建，便于并行开发时多人共用。
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd)"
LOCK="${MIO_BUILD_LOCK:-/tmp/mio_build.lock}"
FILTER="${1:-}"

echo "== 构建 MIO =="
flock "$LOCK" xmake build MIO >/tmp/mio_build_main.log 2>&1
rc_main=$?
tail -3 /tmp/mio_build_main.log

echo "== 构建 mio_tests =="
flock "$LOCK" xmake build mio_tests >/tmp/mio_build_tests.log 2>&1
rc_tests=$?
tail -3 /tmp/mio_build_tests.log

if [ $rc_main -ne 0 ] || [ $rc_tests -ne 0 ]; then
    echo "-- 构建失败，错误摘要 --"
    grep -E "error:" /tmp/mio_build_main.log /tmp/mio_build_tests.log | head -40
    exit 1
fi

BIN="$ROOT/build/linux/x86_64/release/mio_tests"
echo "== 运行验收用例 =="
"$BIN" "$FILTER"
