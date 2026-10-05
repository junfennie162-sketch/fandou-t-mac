#!/system/bin/sh
# lut_evidence2.sh —— 重活那一段（模型加载/生成/鲁棒性）单独一个一次性服务：
# 与 lut_evidence.sh 拆开是因为**服务有时间预算**（FIX-67/71）——一个服务扛完所有分段会被 watchdog 掐。
echo "#####LUT-EV2-BEGIN#####" > /dev/ttyS0
sleep 75

F=/data/local/tmp/lut_evidence2.txt
mkdir -p /data/local/tmp 2>/dev/null

{
  echo "#####LUT-EV2-START#####"
  date
  echo "--- [1] 客户端经 samgr 调用 SA 6901（真推理：LoadModel + Generate×3）---"
  echo "#####LUT-EV-STEP-2-client-start#####" > /dev/ttyS0
  /system/bin/lut_sa_client "$M"
  echo "#####LUT-EV-STEP-3-client-done#####" > /dev/ttyS0

  echo "--- [1b] 第二个模型：标准 ggml 量化（不带 t-mac 类型，走 ggml 通用内核）---"
  Q=/data/local/tmp/qwen.gguf
  if [ -f "$Q" ]; then
    chmod 644 "$Q" 2>/dev/null
    chcon u:object_r:system_file:s0 "$Q" 2>&1 | head -2
    /system/bin/lut_sa_client "$Q"
  else
    echo "  缺 $Q（未注入第二个模型）"
  fi

  echo "--- [6] 接口鲁棒性（--stress：重复调用/坏路径/未加载推理/Release 后调用）---"
  /system/bin/lut_sa_client --stress

  echo "#####LUT-EV2-END#####"
} > $F 2>&1

cat $F > /dev/ttyS0 2>&1
