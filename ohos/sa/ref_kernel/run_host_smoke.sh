#!/usr/bin/env bash
# 主机侧全流程自测（无需设备）：g++ 编译 SA 业务内核 + 便携参考内核并跑冒烟
# 用法: bash run_host_smoke.sh [仓库 ohos 目录]   （默认按脚本位置自动推断）
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"     # .../ohos/sa/ref_kernel
S="$(cd "$HERE/.." && pwd)"               # .../ohos/sa
S="${1:-$S}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp "$HERE/sa_smoke_main.cpp" "$WORK/main.cpp"

echo "== 编译（g++ -std=c++17 -O1） =="
g++ -std=c++17 -O1 -Wall -I"$S" -I"$S/../sched" -I"$S/ref_kernel" -I"$S/ref_kernel/t-mac" \
  "$S/lut_sa.cpp" "$S/session_workspace.cpp" "$S/tile_compute.cpp" "$S/tile_pool.cpp" \
  "$S/../sched/qos_policy.cpp" "$S/ref_kernel/lut_kernel_ref.cpp" "$WORK/main.cpp" \
  -o "$WORK/sa_smoke" || { echo "❌ 编译失败"; exit 1; }

echo "== 运行 =="
"$WORK/sa_smoke"
rc=$?
[ $rc -eq 0 ] && echo "✅ 主机侧自测通过" || echo "❌ 主机侧自测失败 (rc=$rc)"
exit $rc
