#!/usr/bin/env bash
# build_supervisor.sh —— 自愈式构建守护（无人值守跑完 OpenHarmony 产品构建）
#
# 为什么需要它：本机 WSL 上，构建进程树**会被会话/作业对象意外整棵杀掉**（实测 11:07:38 突然死亡，
# 日志停在 [3350/30744]，无任何错误横幅，distro 本身没重启）。固定的一次性启动挡不住这种死法。
#
# 做法：
#   - 循环重启构建（ninja 增量续编，重启只重做在飞目标，代价小）
#   - 每 20 s 写心跳文件 /src/supervisor.heartbeat，供"是否已有守护在跑"跨会话判断
#   - **区分死法**：日志里出现 hb 的错误横幅 `=====build  error=====` 说明是真编译错误 →
#     写 /src/build_hard_error 并停止自动重启（避免拿同一错误空转，把问题藏起来）
#   - 镜像齐了（system.img 出现）就退出
#
# 用法： bash build_supervisor.sh
set -uo pipefail

OH=/src/ohos
IMG=$OH/out/x86_64_virt/packages/phone/images
LOG=/src/build64.log
HB=/src/supervisor.heartbeat
HARD=/src/build_hard_error

cd "$OH" || exit 1
export HOME=/root
unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY

echo $$ > /src/supervisor.pid
echo "[sup] supervisor 启动 pid=$$ $(date '+%F %T')" >> "$LOG"

# 心跳线程（20 s 一次；优先用 python 计时，WSL 的 sleep 曾出现失效）
(
  while :; do
    date +%s > "$HB" 2>/dev/null
    python3 -c 'import time; time.sleep(20)' 2>/dev/null || sleep 20
  done
) &
HB_PID=$!
trap 'kill $HB_PID 2>/dev/null' EXIT

ROUND=0
while :; do
  # 启动前检查硬错误：systemd 重启守护时不要拿同一个错误空转
  if [ -f "$HARD" ]; then
    echo "[sup] 存在硬错误标记 $HARD，守护退出（等人工修复）" >> "$LOG"
    break
  fi
  date +%s > "$HB"
  ROUND=$((ROUND+1))

  if [ -f "$IMG/system.img" ]; then
    echo "[sup] ✅ 镜像已就位，守护退出 $(date '+%F %T')" >> "$LOG"
    break
  fi

  echo "=== [build_bg] start round=$ROUND $(date '+%F %T') ===" >> "$LOG"
  ./build.sh --product-name x86_64_virt --ccache --ninja-args=-j6 >> "$LOG" 2>&1
  RC=$?
  echo "[sup] round=$ROUND build.sh 退出码=$RC @ $(date '+%F %T')" >> "$LOG"

  if [ -f "$IMG/system.img" ]; then
    echo "[sup] ✅ 镜像已就位（构建正常收尾），守护退出" >> "$LOG"
    break
  fi

  # 判死法：真编译错误 = hb 的错误横幅；否则视为"被杀"，重启即可
  if tail -80 "$LOG" | grep -q '=====build  error====='; then
    echo "[sup] ⛔ 检测到 hb 错误横幅（真编译错误），停止自动重启；修好后删除 $HARD 并重跑" >> "$LOG"
    touch "$HARD"
    break
  fi

  echo "[sup] 疑似被杀（无错误横幅），20 s 后重启（ninja 会增量续编）" >> "$LOG"
  python3 -c 'import time; time.sleep(20)' 2>/dev/null || sleep 20
done

echo "[sup] supervisor 结束 $(date '+%F %T')" >> "$LOG"
