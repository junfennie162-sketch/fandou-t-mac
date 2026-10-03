#!/usr/bin/env bash
# ensure_supervisor.sh —— 由「每 5 分钟」计划任务调用（跨 distro 重启的兜底）
#
# 现在主体逻辑在 distro 内的 systemd：
#   ohbuild.service → build_supervisor.sh（构建 + 心跳 + 死法区分）
#   oh-after.service → wait_and_light_up.sh（等镜像 → QEMU → 采证据）
# 本脚本只做三件事：① 把 distro 唤起来（wsl.exe 调用本身就是唤醒） ② 确认 ohbuild 在跑 ③ 有硬错误就不折腾
set -uo pipefail

exec >> /src/supervisor.out 2>&1
echo "===== [ensure] $(date '+%F %T') ====="

IMG=/src/ohos/out/x86_64_virt/packages/phone/images/system.img
HARD=/src/build_hard_error

if [ ! -d /run/systemd/system ]; then
  echo "[ensure] ⚠ systemd 未在运行，退回直接跑守护"
  exec bash /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main/ohos/sa/intree/build_supervisor.sh
fi

if [ -f "$IMG" ]; then
  echo "[ensure] 镜像已就位（$(ls -la "$IMG" | awk '{print $5" 字节 "$6" "$7" "$8}')）"
  systemctl start oh-after.service 2>/dev/null || true
  systemctl is-active ohbuild.service oh-after.service 2>&1 | tr '\n' ' '; echo
  exit 0
fi

if [ -f "$HARD" ]; then
  echo "[ensure] 存在硬错误标记 $HARD —— 等人工修复，不自动重启构建"
  systemctl is-active ohbuild.service 2>&1
  exit 0
fi

systemctl start ohbuild.service
echo "[ensure] ohbuild: $(systemctl is-active ohbuild.service)  主进程 $(systemctl show -p MainPID --value ohbuild.service)"
echo "[ensure] 构建进度: $(grep -oE '\[[0-9]+/[0-9]+\]' /src/build64.log 2>/dev/null | tail -1)"
echo "[ensure] oh-after: $(systemctl is-active oh-after.service)"
