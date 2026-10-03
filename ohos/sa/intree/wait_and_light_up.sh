#!/usr/bin/env bash
# wait_and_light_up.sh —— 无人值守：等产品镜像 → 起 QEMU → 采「点亮三条证据」
#
# 与 after_build.sh 的区别（都是踩过的坑）：
#   1) 镜像判据用 system.img/vendor.img/userdata.img（bzImage 早就存在，用它判断会误判"镜像就位"）
#   2) 起 QEMU 后**轮询等设备就绪**（TCG 无 KVM，OH 冷启动常需 3–10 分钟），而不是盲 sleep 90 s
#   3) 全程日志落 /src/after_build.log，可反复跑（先杀旧 QEMU）
#
# 用法： bash wait_and_light_up.sh [等待构建结束=1|不等=0]
set -uo pipefail

SA=/mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main/ohos/sa
IMG=/src/ohos/out/x86_64_virt/packages/phone/images
LOG=/src/after_build.log
SERIAL=/src/oh-serial.log
HDC="/mnt/d/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe"
TARGET=127.0.0.1:8710
WAIT_BUILD=${1:-1}

log() { echo "[$(date '+%F %T')] $*"; }

# WSL 曾出现 sleep 失效（内核定时器坏），这里优先用 python 计时
nap() { python3 -c 'import time,sys; time.sleep(float(sys.argv[1]))' "$1" 2>/dev/null || sleep "$1"; }

# 不用 pgrep（计划任务拉起的会话里 PATH/行为不确定，曾导致"立即判定构建已退出"）：
# 直接扫 /proc/*/cmdline，只依赖 /proc 与 grep
build_running() {
  local p
  for p in /proc/[0-9]*/cmdline; do
    [ -r "$p" ] || continue
    if tr '\0' ' ' < "$p" 2>/dev/null | grep -q 'build\.sh --product-name'; then
      return 0
    fi
  done
  return 1
}

exec >> "$LOG" 2>&1

log "================ 启动守候 ================"
log "自检: pgrep=$(command -v pgrep || echo 无)  /proc 可读=$([ -r /proc/1/cmdline ] && echo 是 || echo 否)  当前构建在跑=$([ "$(build_running && echo yes)" = yes ] && echo 是 || echo 否)"

if [ "$WAIT_BUILD" = "1" ]; then
  log "等待产品构建结束（扫 /proc 查 build.sh --product-name）..."
  n=0
  while build_running; do
    nap 30; n=$((n+1))
    # 若镜像已就位，也不必等进程退出（收到 build.sh 收尾动作时）
    [ -f "$IMG/system.img" ] && { log "  检测到 system.img 已就位，提前结束等待"; break; }
    if [ $((n % 10)) -eq 0 ]; then
      log "  仍在构建：$(grep -oE '\[[0-9]+/[0-9]+\]' /src/build64.log 2>/dev/null | tail -1)  已等 $((n*30/60)) 分钟"
    fi
  done
  log "构建进程已退出（或镜像已就位），等 30 s 让文件落盘"
  nap 30
fi

log "--- build64.log 尾部 15 行 ---"
tail -15 /src/build64.log 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g'
if [ -f /src/ohos/out/x86_64_virt/error.log ]; then
  log "--- error.log 尾部 15 行 ---"
  tail -15 /src/ohos/out/x86_64_virt/error.log | sed 's/\x1b\[[0-9;]*m//g'
fi

log "--- 镜像目录 ---"
ls -la "$IMG"

MISSING=0
for f in bzImage ramdisk.img system.img vendor.img userdata.img; do
  [ -f "$IMG/$f" ] || { log "❌ 缺 $f"; MISSING=1; }
done
if [ "$MISSING" = "1" ]; then
  log "❌ 镜像不完整 —— 构建没成功，等修完再跑本脚本"
  exit 1
fi
log "✅ 镜像齐全"

# 顺手确认 SA 是否编出来 / 进了镜像树
log "--- SA 产物 ---"
find /src/ohos/out/x86_64_virt -name 'libtmac_sa*' 2>/dev/null | head
ls -la /src/ohos/out/x86_64_virt/packages/phone/system/lib64/ 2>/dev/null | grep -i tmac
ls -la /src/ohos/out/x86_64_virt/packages/phone/system/profile/ 2>/dev/null | grep -i lut
ls -la /src/ohos/out/x86_64_virt/packages/phone/system/etc/init/ 2>/dev/null | grep -i lut

# 杀旧 QEMU，重新起一个
pkill -f 'qemu-system-x86_64' 2>/dev/null
nap 3

log "启动 QEMU（headless + 串口落盘 + monitor socket）"
bash "$SA/intree/qemu_boot_lutsa.sh" daemon
nap 20
pgrep -af 'qemu-system' | head -2

log "轮询等设备就绪（最多 20 分钟）"
READY=0
for i in $(seq 1 80); do
  if "$HDC" -t "$TARGET" shell "echo ready" 2>/dev/null | grep -q ready; then
    log "  ✅ 设备就绪（第 $((i*15)) 秒）"; READY=1; break
  fi
  if [ $((i % 4)) -eq 0 ]; then
    log "  ...第 $((i*15)) 秒，串口进度：$(grep -acE 'Init|init' "$SERIAL" 2>/dev/null) 行含 init｜最后一行: $(tail -1 "$SERIAL" 2>/dev/null | cut -c1-120)"
  fi
  nap 15
done

if [ "$READY" = "0" ]; then
  log "⚠ 20 分钟内设备未就绪；仍尝试采集证据（可能串口里已有 SA 日志）"
fi

log "采集证据 → /src/lutsa-evidence"
"$HDC" tconn "$TARGET" >/dev/null 2>&1
bash "$SA/intree/qemu_collect_evidence.sh" "$HDC" /src/lutsa-evidence

log "--- 证据判定 ---"
for f in /src/lutsa-evidence/01-hidumper-ls.txt /src/lutsa-evidence/02-hidumper-s6901.txt /src/lutsa-evidence/03-hilog-lutsa.txt; do
  if [ -s "$f" ]; then
    log "  $(basename "$f"): $(wc -l < "$f") 行; 头部: $(head -3 "$f" | tr '\n' ' ' | cut -c1-160)"
  else
    log "  $(basename "$f"): 空/不存在"
  fi
done
grep -qi 'LutSystemAbility' /src/lutsa-evidence/01-hidumper-ls.txt 2>/dev/null && log "  ✅ 01 出现 LutSystemAbility(6901)"
grep -q 'Publish' /src/lutsa-evidence/03-hilog-lutsa.txt 2>/dev/null && log "  ✅ 03 出现 Publish"
log "================ 守候结束 ================"
