#!/system/bin/sh
# lut_evidence2.sh —— 重活那一段（模型加载/生成/鲁棒性）单独一个一次性服务：
# 与 lut_evidence.sh 拆开是因为**服务有时间预算**（FIX-67/71）——一个服务扛完所有分段会被 watchdog 掐。
echo "#####LUT-EV2-BEGIN#####" > /dev/ttyS0
sleep 75

F=/data/local/tmp/lut_evidence2.txt
M=/data/local/tmp/model.gguf
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

  echo "--- [1c] 模型路径的意图解析（S6-2）：模型已加载后再问一次 ---"
  echo "[1c1] “打开设置” → 期望 ErrCode=0；source 应为 model（模型路径）或 keyword（回退，均需如实）"
  /system/bin/lut_sa_client --intent "打开设置" "$M"
  echo "[1c2] “打开相机” → 期望非 0（严格校验：模型随口说的包名若不表内/未授权，一律不认）"
  /system/bin/lut_sa_client --intent "打开相机" "$M"

  echo "--- [1d] 引擎存活性实验：跨 IPC 调用引擎还在不在（隔离"模型分类"这件事）---"
  echo "[1d1] 只加载不释放"
  /system/bin/lut_sa_client --load-keep "$M"
  echo "[1d2] 另起一个客户端进程，只读指标 → 看 engine= 是否还是 ready"
  /system/bin/lut_sa_client --metrics
  echo "[1d3] 再问一次意图（同一个已加载引擎）→ 期望 source=model 或带原因的 keyword"
  /system/bin/lut_sa_client --intent "打开设置"
  echo "[1d4] 收尾：只读指标（看实验结束时引擎状态）"
  /system/bin/lut_sa_client --metrics

  echo "--- [6] 接口鲁棒性（--stress：重复调用/坏路径/未加载推理/Release 后调用）---"
  /system/bin/lut_sa_client --stress

  echo "#####LUT-EV2-END#####"
} > $F 2>&1

cat $F > /dev/ttyS0 2>&1
