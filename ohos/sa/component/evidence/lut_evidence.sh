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

  echo "--- [9] 系统级动作执行（S6-1b-2）：start_ability 参数变体一轮试完 ---"
  printf 'com.ohos.settings
' > /data/lut_sa/actions_allow.txt
  echo "[9a1] 只给 bundle（基线，已知 AMS=22）"
  /system/bin/lut_sa_client --action start_ability com.ohos.settings
  echo "[9a2] 显式 ability（入口 com.ohos.settings.MainAbility）→ 期望 AMS 受理"
  /system/bin/lut_sa_client --action start_ability com.ohos.settings/com.ohos.settings.MainAbility
  echo "[9a3] 显式 ability + userId=100"
  /system/bin/lut_sa_client --action start_ability com.ohos.settings/com.ohos.settings.MainAbility@100
  echo "[9a4] 显式 ability + userId=100 + module=phone（module 名取自 Settings.hap 的 module.json）"
  /system/bin/lut_sa_client --action start_ability 'com.ohos.settings/com.ohos.settings.MainAbility@100#phone'
  echo "[9b] 未授权目标（com.ohos.camera）→ 期望 201"
  /system/bin/lut_sa_client --action start_ability com.ohos.camera
  echo "[9c] 删表恢复（默认只放 settings）→ 期望受理（同 [9a4]）"
  rm -f /data/lut_sa/actions_allow.txt
  /system/bin/lut_sa_client --action start_ability 'com.ohos.settings/com.ohos.settings.MainAbility@100#phone'

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

  echo "--- [10] 自然语言 → 动作（S6-2）：source 字段如实标注 ---"
  echo "[10a] “打开设置” → 期望 ErrCode=0 且 bundle=com.ohos.settings（轻活服务里没加载模型 → source=keyword）"
  /system/bin/lut_sa_client --intent "打开设置"
  echo "[10b] “打开相机” → 期望拒绝（动作表没匹配）"
  /system/bin/lut_sa_client --intent "打开相机"
  echo "[10c] 自定义动作表把“打开设置”指向未授权 bundle(com.ohos.camera) → 期望 201（动作白名单仍然生效）"
  printf '打开设置=com.ohos.camera
' > /data/lut_sa/intents.txt
  /system/bin/lut_sa_client --intent "打开设置"
  rm -f /data/lut_sa/intents.txt
  echo "[10d] 删表回默认 → 期望再回到 0"
  /system/bin/lut_sa_client --intent "打开设置"
  echo "[10e] 内置表已含"看相机"→com.ohos.camera，但动作白名单默认没放它 → 期望 201（两道门）"
  /system/bin/lut_sa_client --intent "看相机"

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

  echo "--- [8] 配额（S5-2）：可加载模型大小上限 ---"
  echo "[8a] 写 model_mb=1（上限 1MB）+ 一次 966MB 模型的加载 → 期望 LoadModel 拒绝、SA 存活"
  echo "model_mb=1" > /data/lut_sa/quota.txt
  /system/bin/lut_sa_client --load "$M"
  echo "[8b] 删掉配额配置 → 期望重新加载成功（ErrCode=0）"
  rm -f /data/lut_sa/quota.txt
  /system/bin/lut_sa_client --load "$M"

  echo "#####LUT-EV-END#####"
} > $F 2>&1

cat $F > /dev/ttyS0 2>&1
