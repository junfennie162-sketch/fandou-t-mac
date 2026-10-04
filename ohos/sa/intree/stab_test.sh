#!/bin/bash
# stab_test.sh —— LUT-SA SystemAbility 稳定性测试：多轮冷启动，逐轮验证「注册 + 全接口调用」
# 判据来自 guest 内的客户端（lut_sa_client）输出，不是宿主机猜的
# 用法: bash stab_test.sh [轮数]
R=${1:-3}
OUT=/mnt/c/Users/NJF/Desktop/t-mac/wsl/stab_report.txt
LOG=/src/oh-serial.log

: >> "$OUT"
{
  echo "===== LUT-SA SA 稳定性测试 ====="
  echo "轮数: $R   开始: $(date '+%F %T')"
  echo "判据: guest 内 /system/bin/lut_sa_client 的输出"
  echo
  printf '%-6s %-16s %-12s %-10s %-8s %-10s %s\n' "轮次" "GetSystemAbility" "NativeVersion" "SelfTest" "失败项" "客户端rc" "panic"
} >> "$OUT"

pass=0
for i in $(seq 1 "$R"); do
  # 直接重启 QEMU（boot 脚本会 : > $LOG 清空串口日志 → 标记只能来自本轮，避免吃到上一轮结果）
  pkill -f qemu-system-x86_64 2>/dev/null; sleep 3
  : > "$LOG"
  bash "/mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main/ohos/sa/intree/qemu_boot_lutsa.sh" daemon >/dev/null 2>&1
  echo "  第 $i 轮：QEMU 已重启（$(date '+%H:%M:%S')），等取证..." >&2

  # 等本轮取证输出（最多 4 分钟）
  for w in $(seq 1 20); do
    sleep 12
    grep -aq 'LUTSA-CLIENT-END' "$LOG" 2>/dev/null && break
  done
  sleep 5

  r_ok=$(grep -ac '\[GetSystemAbility(6901)\] ok' "$LOG")
  r_ver=$(grep -ac 'LUT-SA native | SA mode' "$LOG")
  r_st=$(grep -ac 'PASS: LUT kernel warm-up' "$LOG")
  r_fail=$(grep -ao '失败项 [0-9]*' "$LOG" | tail -1 | grep -o '[0-9]*')
  r_rc=$(grep -ao 'LUTSA-CLIENT-END (rc=[0-9]*)' "$LOG" | tail -1 | grep -o '[0-9]*')
  [ -z "$r_rc" ] && r_rc="?" 
  r_panic=$(grep -ac 'Kernel panic' "$LOG")
  r_boot=$(grep -ac 'ServiceExec lut_sa' "$LOG")

  [ -z "$r_fail" ] && r_fail="?"
  gs="失败"; [ "$r_ok" -ge 1 ] && gs="ok"
  nv="失败"; [ "$r_ver" -ge 1 ] && nv="ok"
  st="失败"; [ "$r_st" -ge 1 ] && st="PASS"
  pr="无"; [ "$r_proc" -ge 1 ] && pr="有"
  pc="无"; [ "$r_panic" -ge 1 ] && pc="有"

  printf '%-6s %-16s %-12s %-10s %-8s %-10s %s\n' "$i" "$gs" "$nv" "$st" "$r_fail" "$pr" "$pc" >> "$OUT"
  if [ "$gs" = "ok" ] && [ "$nv" = "ok" ] && [ "$st" = "PASS" ] && [ "$r_fail" = "0" ] && [ "$pc" = "无" ]; then
    pass=$((pass+1))
  fi
  echo "  第 $i 轮: GetSA=$gs ver=$nv selftest=$st 失败项=$r_fail SA进程=$pr panic=$pc （启动痕迹 $r_boot）"
done

{
  echo
  echo "===== 汇总 ====="
  echo "通过: $pass / $R"
  echo "结束: $(date '+%F %T')"
} >> "$OUT"
echo
echo "报告: $OUT"
cat "$OUT" | tail -12
