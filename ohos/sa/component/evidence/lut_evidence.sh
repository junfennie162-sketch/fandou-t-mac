#!/system/bin/sh
# lut_evidence.sh —— 开机一次性取证：跑 SA 客户端并打出关键诊断，结果整体打到串口
#
# 两个通道，互为备份：
#   ① 串口直接打标记（ttyS0）—— 脚本卡在哪一步都能看出来（STA-3 踩过：脚本整段没输出，
#      分不清是"没跑"还是"卡住了"）
#   ② 全量结果落 /data/local/tmp/lut_evidence.txt，最后 cat 到串口；
#      即使脚本没跑完，宿主机也能在 QEMU 停机后从 userdata 镜像里把文件捞出来
echo "#####LUT-EV-BEGIN#####" > /dev/ttyS0
sleep 30
echo "#####LUT-EV-STEP-1-sleep-done#####" > /dev/ttyS0

F=/data/local/tmp/lut_evidence.txt
mkdir -p /data/local/tmp 2>/dev/null

{
  echo "#####LUT-EV-START#####"
  date

  echo "--- [0] SELinux 状态与进程上下文（诊断为什么 get 被拒）---"
  echo "enforce=$(cat /sys/fs/selinux/enforce 2>&1)"
  echo "self context: $(cat /proc/self/attr/current 2>&1)"
  # 注意：guest 的 toybox 里没有 awk/tr，只能用 sed（这里踩过：awk: inaccessible or not found）
  for p in $(ps -ef 2>/dev/null | grep -E 'lut_sa|lut_sa_client' | sed -E 's/^[^ ]+[ ]+([0-9]+).*/\1/'); do
    echo "pid $p ($(cat /proc/$p/comm 2>/dev/null)): $(cat /proc/$p/attr/current 2>&1)"
  done

  echo "--- [0b] 模型就位与 SELinux 标签（SA 进程域要能读 gguf）---"
  M=/data/local/tmp/model.gguf
  if [ -f "$M" ]; then
    echo "path=$M size=$(stat -c %s "$M" 2>&1) magic=$(head -c 4 "$M" 2>/dev/null | od -An -c 2>&1)"
    echo "label-before: $(ls -lZ "$M" 2>&1 | head -1)"
    chmod 644 "$M" 2>/dev/null
    # 证据服务以 init 身份运行 → 能改标签。统一成 system_file（各域普遍持有 read/open/map）
    chcon u:object_r:system_file:s0 "$M" 2>&1 | head -2
    echo "label-after : $(ls -lZ "$M" 2>&1 | head -1)"
    head -c 8 "$M" > /dev/null 2>&1 && echo "self-read   : ok" || echo "self-read   : FAIL"
  else
    echo "  缺 $M（模型未注入镜像）"
  fi

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

  echo "--- [2] ps ---"
  ps -ef 2>&1 | grep -i lut

  echo "--- [3] 装机文件 ---"
  ls -la /system/lib64/libtmac_sa.z.so /system/bin/lut_sa_client /system/profile/lut_sa.json 2>&1

  echo "--- [4] service_contexts 里的 6901 ---"
  grep -a 6901 /system/etc/selinux/targeted/contexts/service_contexts 2>&1

  echo "--- [5a] hilog 里的 [LutSa]（SA 自己的日志）---"
  hilog -x 2>&1 | grep -a '\[LutSa\]' | head -15
  echo "--- [5b] hilog 里的 SA_CLIENT/Selinux ---"
  hilog -x 2>&1 | grep -aiE 'SA_CLIENT|samgr_class' | head -12

  echo "--- [5c] SA 进程的 stderr/stdout（引擎断言/LOG(FATAL) 都打在这里）---"
  for f in /data/local/tmp/lut_sa_stderr.txt /data/local/tmp/lut_sa_stdout.txt; do
    echo "== $f ($(stat -c %s $f 2>/dev/null) 字节) 尾 25 行 =="
    tail -25 "$f" 2>&1
  done

  echo "--- [6] 接口鲁棒性（--stress：重复调用/坏路径/未加载推理/Release 后调用）---"
  /system/bin/lut_sa_client --stress

  echo "--- [7] 调用方准入（S5-1）：默认档位 vs 白名单（同一进程、不重启 SA）---"
  rm -f /data/lut_sa/allow_uids.txt 2>/dev/null
  mkdir -p /data/lut_sa 2>/dev/null
  echo "[7a] 默认档位（root 属特权 uid）→ 期望 SelfTest 放行、失败项 0"
  /system/bin/lut_sa_client
  echo "[7b] 写白名单只放 uid 12345（root 不在内）→ 期望 SelfTest 返回 201、失败项 ≥1"
  echo "12345" > /data/lut_sa/allow_uids.txt
  /system/bin/lut_sa_client
  echo "[7c] 删掉白名单恢复默认档位 → 期望重新放行（证明策略每次调用都重读）"
  rm -f /data/lut_sa/allow_uids.txt
  /system/bin/lut_sa_client
  echo "#####LUT-EV-END#####"
} > $F 2>&1

cat $F > /dev/ttyS0 2>&1
