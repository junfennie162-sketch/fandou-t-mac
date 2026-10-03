#!/usr/bin/env bash
# keeper.sh —— 拉住一个 wsl.exe 会话不放，让 distro 不被 WSL 关掉
#
# 根因（实测）：本机 WSL 3.0.1.0 下，**最后一个 wsl.exe 会话结束后约 1 分钟，distro 就会被关掉**
# （`ps -p 1 -o lstart=` 每隔几分钟就是新时间；dmesg 里反复 `systemd-shutdow` + 根文件系统卸载/重挂），
# 即使 distro 内有 systemd 服务在跑也照关 → 构建进程树随之灰飞烟灭。
# 光靠 systemd 服务不够，必须有一个**常驻会话**把 distro 钉住。
#
# 本脚本由「每 5 分钟」的计划任务拉起（任务设置 MultipleInstances=IgnoreNew）：
#   ① 唤醒 distro（wsl.exe 调用本身就会拉起）② 确认两个 systemd 服务在位 ③ 然后长驻不退出
set -uo pipefail

exec >> /src/keeper.out 2>&1
echo "===== [keeper] $(date '+%F %T') 会话建立 PID1=$(ps -p 1 -o lstart=) ====="

echo "[keeper] ohbuild=$(systemctl is-active ohbuild.service 2>/dev/null) oh-after=$(systemctl is-active oh-after.service 2>/dev/null)"

# 长驻：每 5 分钟写一行审计日志（也顺便证明会话还活着）
while :; do
  python3 -c 'import time; time.sleep(300)' 2>/dev/null || sleep 300
  echo "[keeper] $(date '+%F %T') PID1=$(ps -p 1 -o lstart=) ohbuild=$(systemctl is-active ohbuild.service 2>/dev/null) 进度=$(grep -oE '\[[0-9]+/[0-9]+\]' /src/build64.log 2>/dev/null | tail -1)" 
done
