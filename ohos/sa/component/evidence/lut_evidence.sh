#!/system/bin/sh
# lut_evidence.sh —— 开机一次性取证：跑 SA 客户端并打出关键诊断，结果整体打到串口
sleep 40

F=/data/local/tmp/lut_evidence.txt
mkdir -p /data/local/tmp 2>/dev/null

{
  echo "#####LUT-EV-START#####"
  date

  echo "--- [0] SELinux 状态与进程上下文（诊断为什么 get 被拒）---"
  echo "enforce=$(cat /sys/fs/selinux/enforce 2>&1)"
  echo "self context: $(cat /proc/self/attr/current 2>&1)"
  for p in $(ps -ef 2>/dev/null | grep -E 'lut_sa|lut_sa_client' | awk '{print $2}'); do
    echo "pid $p ($(cat /proc/$p/comm 2>/dev/null)): $(cat /proc/$p/attr/current 2>&1)"
  done

  echo "--- [1] 客户端经 samgr 调用 SA 6901（IPC）---"
  /system/bin/lut_sa_client

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

  echo "#####LUT-EV-END#####"
} > $F 2>&1

cat $F > /dev/ttyS0 2>&1
