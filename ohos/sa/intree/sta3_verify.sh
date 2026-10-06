#!/bin/bash
# sta3_verify.sh —— 一键验证（在 WSL 里跑；宿主机侧脚本，仓库内镜像）
# 用法: MSYS_NO_PATHCONV=1 wsl.exe -d ohbuild -u root -- bash -c 'EVOUT=<证据路径> bash <此文件>'

#!/bin/bash
# STA-3：把 llama.cpp 静态链进 SA（真 LoadModel / 真 Generate），模型入 userdata 镜像，冷启动取证
OUT=/mnt/c/Users/NJF/Desktop/t-mac/wsl/sta3.txt
exec > "$OUT" 2>&1
SA=/mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main/ohos/sa
O=/src/ohos/out/x86_64_virt
N=/src/ohos/prebuilts/build-tools/linux-x86/bin/ninja
IMG=$O/packages/phone/images
M=/mnt/sysimg
UD=/mnt/udata
LOG=/src/oh-serial.log
MODEL_SRC=/mnt/d/ohos-models/bitnet-3b-tmac.gguf

echo "=== 1. 同步源码到树（引擎源码 + t-mac 内核 + 引擎静态库）==="
bash "$SA/intree/install_into_tree.sh" /src/ohos "$SA" > /src/install_tree.log 2>&1
echo "  同步+编译退出码: $?"
grep -aE '引擎|llama 源码|t-mac 头|LUT 内核|kernels.cc 覆盖|kcfg.ini 覆盖|arm64 内核|kernels.h|已追加|挂载|\[C \]|\[C\+\+ \]|dmlc shim' /src/install_tree.log | cut -c1-200
echo "  -- 引擎编译失败详情（若有）--"
grep -aE "error:" /src/install_tree.log | head -8 | cut -c1-200

echo
echo "=== 2. 重建 gn + 链接 SA（含静态引擎）==="
# gn 重生成必须带 -w dupbuild=warn：上游有个重复生成 contacts_data stamp 的规则，
# 不加这个开关 gn 直接失败 → build.ninja 不更新 → 编译还用旧 include_dirs（本次踩过）
"$N" -w dupbuild=warn -C $O build.ninja 2>&1 | tail -3
cd $O || exit 1
"$N" -w dupbuild=warn -C $O ohemu/lutsa/libtmac_sa.z.so ohemu/lutsa/lut_sa_client ohemu/lutsa/lut_a11y_dump -j6 2>&1 \
  | grep -vE '^ninja: warning: multiple rules' | tail -45
echo "  ninja 退出码: ${PIPESTATUS[0]}"
ls -la ohemu/lutsa/libtmac_sa.z.so ohemu/lutsa/lut_sa_client ohemu/lutsa/lut_a11y_dump 2>/dev/null | awk '{print "  ", $5, $9}'
echo "  引擎符号（.so 里应能看到 llama_*）:"
nm -D ohemu/lutsa/libtmac_sa.z.so 2>/dev/null | grep -c "llama_" | xargs echo "    llama_* 导出:"
strings -a ohemu/lutsa/libtmac_sa.z.so 2>/dev/null | grep -c "ggml-tmac\|tmac" | xargs echo "    tmac 相关字符串:"

echo
echo "=== 3. 注入 system.img（库 + 客户端 + 取证脚本 + kcfg）==="
mkdir -p $M
mount -o loop,rw "$IMG/system.img" $M || exit 1
cp -f $O/ohemu/lutsa/libtmac_sa.z.so $M/system/lib64/libtmac_sa.z.so
cp -f $O/ohemu/lutsa/lut_sa_client  $M/system/bin/lut_sa_client && chmod 755 $M/system/bin/lut_sa_client
cp -f $O/ohemu/lutsa/lut_a11y_dump  $M/system/bin/lut_a11y_dump && chmod 755 $M/system/bin/lut_a11y_dump
# 取证脚本/cfg **按 glob 全量注入**（FIX-73：手写文件清单会漏 —— 第二个服务就是这么没起来的）
cp -f $SA/component/evidence/lut_evidence*.sh $M/system/bin/ && chmod 755 $M/system/bin/lut_evidence*.sh
cp -f $SA/component/etc/init/lut_evidence*.cfg $M/system/etc/init/
cp -f $SA/component/etc/init/lut_sa.cfg $M/system/etc/init/lut_sa.cfg   # FIX-87: SA 的 cfg 也要注入（权限就在它里面；漏了会 1005）
mkdir -p $M/system/etc/lut_sa
cp -f /src/ohos/vendor/ohemu/lutsa/etc/lut_sa/kcfg.ini $M/system/etc/lut_sa/kcfg.ini
echo "  已注入取证脚本: $(ls $M/system/bin/lut_evidence*.sh | xargs -n1 basename | tr '
' ' ')"
echo "  已注入 init cfg : $(ls $M/system/etc/init/lut_evidence*.cfg | xargs -n1 basename | tr '
' ' ')"
mkdir -p $M/system/etc/lut_sa
cp -f /src/ohos/vendor/ohemu/lutsa/etc/lut_sa/kcfg.ini $M/system/etc/lut_sa/kcfg.ini
echo "  库 $(stat -c %s $M/system/lib64/libtmac_sa.z.so) / 客户端 $(stat -c %s $M/system/bin/lut_sa_client) / kcfg $(stat -c %s $M/system/etc/lut_sa/kcfg.ini 2>/dev/null)"
df -h $M | tail -1
sync; umount $M && echo "  已卸载"

echo
echo "=== 4. 注入模型到 userdata.img（966 MB）==="
e2fsck -fy "$IMG/userdata.img" >/dev/null 2>&1
mkdir -p $UD
mount -o loop,rw "$IMG/userdata.img" $UD || exit 1
mkdir -p $UD/local/tmp
NEED=$(ls -l "$MODEL_SRC" | sed -E 's/^[^ ]+ +[0-9]+ +[^ ]+ +[^ ]+ +([0-9]+).*/\1/')
HAVE=$(ls -l "$UD/local/tmp/model.gguf" 2>/dev/null | sed -E 's/^[^ ]+ +[0-9]+ +[^ ]+ +[^ ]+ +([0-9]+).*/\1/')
if [ "$NEED" = "$HAVE" ]; then
  echo "  已存在且大小一致（$HAVE 字节）→ 跳过拷贝"
else
  echo "  拷贝 $NEED 字节（约 1 分钟）…"
  cp -f "$MODEL_SRC" $UD/local/tmp/model.gguf
fi
echo "  模型大小: $(stat -c %s $UD/local/tmp/model.gguf) 字节"
# 第二个模型：标准 ggml 量化（Qwen2.5-0.5B q4_k_m，491 MB）——用它把"引擎真能推理"这条链路
# 与"t-mac 2bit 模型的形状表匹配"这件事解耦（后者是模型转换侧的问题）
QSRC=/mnt/d/ohos-models/qwen2.5-0.5b-instruct-q4_k_m.gguf
if [ -f "$QSRC" ]; then
  if [ "$(stat -c %s "$QSRC")" = "$(stat -c %s $UD/local/tmp/qwen.gguf 2>/dev/null)" ]; then
    echo "  qwen.gguf 已存在且大小一致 → 跳过拷贝"
  else
    cp -f "$QSRC" $UD/local/tmp/qwen.gguf
    echo "  已注入 qwen.gguf $(stat -c %s $UD/local/tmp/qwen.gguf) 字节"
  fi
fi
# SA 进程以 system uid 运行，要在 /data/lut_sa 里写运行期 stderr（崩溃原因的唯一通道）
mkdir -p $UD/lut_sa
chmod 777 $UD/lut_sa
python3 - <<'PY'
import os
p = '/mnt/udata/local/tmp/model.gguf'
try:
    # 关键：宿主侧直接把 SELinux 标签写进 xattr，免去 guest 里再改一遍
    os.setxattr(p, 'security.selinux', b'u:object_r:system_file:s0')
    print('  xattr security.selinux =', os.getxattr(p, 'security.selinux'))
except Exception as e:
    print('  ⚠ 宿主机 setxattr 失败（改由 guest 内 chcon 兜底）:', e)
print('  /data/lut_sa 权限:', oct(os.stat('/mnt/udata/lut_sa').st_mode & 0o777))
PY
df -h $UD | tail -1
sync; umount $UD && echo "  已卸载"

echo
echo "=== 5. 冷启动并取证 ==="
pkill -f qemu-system-x86_64 2>/dev/null; sleep 3
: > "$LOG"
bash "$SA/intree/qemu_boot_lutsa.sh" daemon >/dev/null 2>&1
for i in $(seq 1 40); do
  sleep 12
  grep -aq 'LUT-EV2-END' "$LOG" 2>/dev/null && grep -aq 'LUT-EV-END' "$LOG" 2>/dev/null && { echo "  取证完成（$((i*12))s）"; break; }
done
echo "串口 $(wc -l < $LOG) 行  panic=$(grep -ac 'Kernel panic' $LOG)"

echo
echo "########## guest 内取证脚本的进度标记 ##########"
grep -a 'LUT-EV-BEGIN\|LUT-EV-STEP' "$LOG" | sed -E 's/^.*(#####LUT-EV[^#]*#####).*$/\1/' | sort -u | head
echo "（只有 BEGIN/STEP-1 说明客户端卡住；有 STEP-3 说明脚本走完了）"

echo
echo "=== 6. 停机后从 userdata 镜像里捞运行期日志（不依赖 guest 脚本跑完）==="
pkill -f qemu-system-x86_64 2>/dev/null; sleep 4
e2fsck -fy "$IMG/userdata.img" >/dev/null 2>&1
mkdir -p /mnt/ud2
mount -o loop,ro "$IMG/userdata.img" /mnt/ud2 2>/dev/null && {
  for f in lut_sa/rt_stderr.txt lut_sa/rt_stdout.txt local/tmp/lut_evidence.txt local/tmp/lut_evidence2.txt; do
    P=/mnt/ud2/$f
    echo "===== $f ($(stat -c %s "$P" 2>/dev/null) 字节) ====="
    [ -f "$P" ] && tail -c 4000 "$P" | tr -d '\000'
    echo
  done
  umount /mnt/ud2
} || echo "  userdata 挂不上（跳过）"

echo
echo "=== 7. 打包仓库证据：evidence/42-sa-real-inference-sta3.txt ==="
EV=${EVOUT:-/mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main/ohos/sa/evidence/42-sa-real-inference-sta3.txt}
mount -o loop,ro "$IMG/userdata.img" /mnt/ud2 2>/dev/null && {
  {
    echo "===== LUT-SA SystemAbility 真推理（STA-3）====="
    echo "环境: OpenHarmony 标准系统镜像（QEMU x86_64_virt）; SA 进程内经 IPC 真跑 llama.cpp"
    echo "抓取: $(date '+%F %T')"
    echo
    echo "--- 引擎形态 ---"
    echo "静态库 libllama_engine.a：llama.cpp + ggml + t-mac LUT 内核，用 OH 树自己的 clang 编"
    echo "（仓库里的预编译 libllama.a/libggml.a 是 DevEco 工具链编的，引用 std::__n1::*，"
    echo " 而 OH libc++ 是 std::__h::* → ABI 不同，链不了；见 QEMU-DEPLOY.md FIX-53）"
    echo
    echo "--- [1] t-mac 2bit 模型（形状表不匹配 → 优雅失败，不再打死 SA）---"
    sed -n '/\[1\] 客户端/,/\[1b\]/p' /mnt/ud2/local/tmp/lut_evidence2.txt | head -30
    echo
    echo "--- [1b] 标准 ggml 模型（Qwen2.5-0.5B q4_k_m）：真加载 + 真出 token ---"
    sed -n '/\[1b\] 第二个模型/,/####/p' /mnt/ud2/local/tmp/lut_evidence2.txt | head -48
    echo
    echo "--- [6] 接口鲁棒性 ---"
    sed -n '/\[6\] 接口鲁棒性/,$p' /mnt/ud2/local/tmp/lut_evidence2.txt | head -16
  echo
  echo "--- [1c] 模型路径的意图解析 ---"
  sed -n '/\[1c\]/,$p' /mnt/ud2/local/tmp/lut_evidence2.txt | head -20
  echo
  echo "--- [1d] 引擎存活性隔离实验 ---"
  sed -n '/\[1d\]/,/\[6\]/p' /mnt/ud2/local/tmp/lut_evidence2.txt | head -24
  echo
  echo "--- [7] 调用方准入（S5-1）：默认档位 / 白名单拒绝 / 删表恢复 ---"
  sed -n '/\[7\] 调用方准入/,$p' /mnt/ud2/local/tmp/lut_evidence.txt | head -40
  echo
  echo "--- [8] 配额（S5-2）：model_mb 上限 / 删配置恢复 ---"
  sed -n '/\[8\] 配额/,$p' /mnt/ud2/local/tmp/lut_evidence.txt | head -36
  echo
  echo "--- [9] 系统级动作执行（S6-1b）：start_ability + 动作白名单 ---"
  sed -n '/\[9\] 系统级动作执行/,$p' /mnt/ud2/local/tmp/lut_evidence.txt | head -30
  echo
  echo "--- [10] 自然语言 → 动作（source 如实）---"
  sed -n '/\[10\] 自然语言/,$p' /mnt/ud2/local/tmp/lut_evidence.txt | head -30
  echo
  echo "--- [11] 动作白名单「正向授权」双证明 ---"
  sed -n '/\[11\]/,/\[12\]/p' /mnt/ud2/local/tmp/lut_evidence.txt | head -20
  echo
  echo "--- [12] S7-0 侦察：无障碍服务状态 ---"
  sed -n '/\[12\]/,/\[7\]/p' /mnt/ud2/local/tmp/lut_evidence.txt | head -8
  echo
  echo "----- [13] S7-1a 无障碍探针（真错误码是结论，不许 head 截断成「没跑」）-----"
  # FIX-76 第三次：head -90 把 271 行的 [13] 段砍断了 —— 探针段用**整段**（271 行），不设小上限
  sed -n '/\[13\]/,/\[7\]/p' /mnt/ud2/local/tmp/lut_evidence.txt | head -400
  echo
  echo "--- [16..19] S7-2-0 动作可行性 / 可交互界面 / 解锁 / 收口测量 ---"
  sed -n '/\[16\]/,/\[7\]/p' /mnt/ud2/local/tmp/lut_evidence.txt | head -320   # FIX-76：行数要跟分段增长（[16..20]）
  echo
  echo "----- [14] S7-1b SA 读屏（ReadScreen：感知方向，独立 IDL 方法）-----"
  sed -n '/\[14\]/,/\[7\]/p' /mnt/ud2/local/tmp/lut_evidence.txt | head -60
  echo
  echo "--- [15] 真机屏幕可视化：截图 + 同步读屏（叠框页面的数据源）---"
  sed -n '/\[15\]/,/\[7\]/p' /mnt/ud2/local/tmp/lut_evidence.txt | head -30
  ls -l /mnt/ud2/local/tmp/lut_screen.* 2>/dev/null | awk '{print "  截图文件:", $5, "字节", $9}'
    echo
    echo "--- SA 进程运行期日志（引擎壳的阶段标记；崩溃原因就在这条通道上）---"
    tail -30 /mnt/ud2/lut_sa/rt_stderr.txt 2>/dev/null
  } > "$EV" 2>&1
  umount /mnt/ud2
  echo "  已写 $EV ($(wc -l < "$EV") 行)"
} || echo "  ⚠ 打不了证据包（userdata 挂不上）"

echo
echo "########## [0b]+[1]：模型标签 + 真推理 ##########"
grep -a -A 46 'LUT-EV-START' "$LOG" | grep -avE 'LoopEvent|SIGCHLD|ServiceReap|appspawn_kickdog|kauditd' | head -48 | cut -c1-210

echo
echo "########## [5c] SA 进程 stderr（崩溃原因在这里）##########"
grep -a -A 30 '\[5c\]' "$LOG" | grep -avE 'LoopEvent|SIGCHLD|ServiceReap|appspawn_kickdog|kauditd' | head -34 | cut -c1-210

echo
echo "########## 串口里的崩溃/退出痕迹 ##########"
grep -aE "lut_sa.*(segfault|abort|SIG|signal)|GGML_ASSERT|LLAMA_ASSERT|Child process lut_sa|exit with code" "$LOG" | tail -12 | cut -c1-200

echo
echo "########## 摘要行 ##########"
grep -aE 'GetSystemAbility\(6901\)|LoadModel\(|Generate#|determinism|non-constant|GetMetrics\(引擎态\)|失败项|engine=' "$LOG" | head -16 | cut -c1-280

echo
echo "########## 引擎/模型相关 avc 拒绝 ##########"
grep -a 'avc:  denied' "$LOG" | grep -aiE 'gguf|lut_sa|compiler|unlabeled|system_file' | head -8 | cut -c1-220

echo
echo "########## SA 进程域 ##########"
grep -aE 'pid [0-9]+ \(lut_sa' "$LOG" | head -4 | cut -c1-160
