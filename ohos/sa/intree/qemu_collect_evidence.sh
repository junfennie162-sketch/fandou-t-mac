#!/usr/bin/env bash
# qemu_collect_evidence.sh —— 镜像编好、QEMU 起来之后，一键采集「点亮三条证据」
#
# 采集项（对应 QEMU-DEPLOY.md §五）：
#   1) hidumper -ls | grep -i lut     → 出现 LutSystemAbility (6901)
#   2) hidumper -s 6901               → 打出组件 banner（kernel: m128-k3200 LUT ...）
#   3) hilog | grep LutSa             → OnStart (SA_ID=6901) / Publish: ok / CreateSession: 0
#   4) 串口日志中的 [LutSa] 行 + 系统界面截图（QEMU monitor screendump）
#
# 前置：QEMU 已用 qemu_boot_lutsa.sh 启动（hdc 8710 已转发、monitor socket 已挂）
# 用法：bash qemu_collect_evidence.sh [hdc路径] [输出目录]
#   默认 hdc 走 DevEco SDK（Windows 侧）："/mnt/d/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe"
set -uo pipefail

HDC=${1:-"/mnt/d/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe"}
OUT=${2:-/src/lutsa-evidence}
SERIAL_LOG=${OH_LOG:-/src/oh-serial.log}
MON=${OH_MON:-/tmp/oh-qemu-mon.sock}
HDC_TARGET="127.0.0.1:8710"

mkdir -p "$OUT"
echo "== 输出目录: $OUT =="

"$HDC" list targets >/dev/null 2>&1 || true
echo "-- 连接设备 $HDC_TARGET"
"$HDC" tconn "$HDC_TARGET" 2>&1 | tee "$OUT/00-hdc-connect.txt"

echo "-- 等待设备就绪（最多 120 s）"
for i in $(seq 1 24); do
  if "$HDC" -t "$HDC_TARGET" shell "echo ready" 2>/dev/null | grep -q ready; then
    echo "   设备就绪（第 ${i} 次探测）"; break
  fi
  sleep 5
done

if ! "$HDC" -t "$HDC_TARGET" shell "echo ready" 2>/dev/null | grep -q ready; then
  echo "❌ 设备未就绪：确认 QEMU 已启动且 8710 已转发（qemu_boot_lutsa.sh daemon）"
  exit 1
fi

echo
echo "== 证据 1: hidumper -ls | grep -i lut =="
"$HDC" -t "$HDC_TARGET" shell "hidumper -ls | grep -i lut" 2>&1 | tee "$OUT/01-hidumper-ls.txt"

echo
echo "== 证据 2: hidumper -s 6901 =="
"$HDC" -t "$HDC_TARGET" shell "hidumper -s 6901" 2>&1 | tee "$OUT/02-hidumper-s6901.txt"

echo
echo "== 证据 3: hilog | grep LutSa =="
"$HDC" -t "$HDC_TARGET" shell "hilog -x | grep -i LutSa | head -40" 2>&1 | tee "$OUT/03-hilog-lutsa.txt"

echo
echo "== 证据 4a: 串口日志里的 [LutSa] =="
if [ -f "$SERIAL_LOG" ]; then
  grep -a 'LutSa' "$SERIAL_LOG" | head -40 | tee "$OUT/04-serial-lutsa.txt"
else
  echo "   ⚠ 未找到串口日志 $SERIAL_LOG"
fi

echo
echo "== 证据 4b: 系统界面截图（QEMU monitor screendump）=="
if command -v socat >/dev/null 2>&1 && [ -S "$MON" ]; then
  echo 'screendump /tmp/oh-screen.ppm' | socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1
  sleep 3
  if [ -f /tmp/oh-screen.ppm ]; then
    mv /tmp/oh-screen.ppm "$OUT/05-oh-screen.ppm"
    echo "   截图已存: $OUT/05-oh-screen.ppm （转 PNG: pnmtopng 或 python3 -c \"from PIL import Image; Image.open('05-oh-screen.ppm').save('05-oh-screen.png')\"）"
  else
    echo "   ⚠ screendump 未产出文件"
  fi
else
  echo "   ⚠ 无 socat 或 monitor socket（$MON）不可用"
fi

echo
echo "== 证据 4c: init 启动痕迹（串口）=="
if [ -f "$SERIAL_LOG" ]; then
  grep -aE 'lut_sa|\[Init\].*lut' "$SERIAL_LOG" | head -30 | tee "$OUT/06-init-lut_sa.txt"
  [ -s "$OUT/06-init-lut_sa.txt" ] || echo "   （串口里没有 lut_sa 的启动痕迹）"
else
  echo "   ⚠ 未找到串口日志 $SERIAL_LOG"
fi

echo
echo "== 证据 4d: SELinux 拒绝（若有，说明服务被策略挡住）=="
if [ -f "$SERIAL_LOG" ]; then
  grep -a 'avc: *denied' "$SERIAL_LOG" | head -30 | tee "$OUT/07-selinux-denials.txt"
  [ -s "$OUT/07-selinux-denials.txt" ] && echo "   ⚠ 有拒绝记录：检查 etc/init/lut_sa.cfg 的 secon 与 sepolicy/*.te 是否一致" \
                                        || echo "   ✓ 无 avc denied"
else
  echo "   ⚠ 未找到串口日志"
fi

echo
echo "== 汇总 =="
for f in "$OUT"/0*.txt; do
  [ -s "$f" ] || continue
  printf '  %-28s %s 行\n' "$(basename "$f")" "$(wc -l < "$f")"
done
echo
echo "判定：01 出现 LutSystemAbility(6901) + 02 打出 banner + 03 有 Publish: ok  →  点亮成功"
