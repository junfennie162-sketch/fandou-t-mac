#!/usr/bin/env bash
# 把 LUT-SA 组件安装进 OpenHarmony 源码树（vendor/ohemu/lutsa），并把组件挂到产品配置。
# 用法: install_into_tree.sh <OH源码树根> <本仓库 ohos/sa 目录>
set -euo pipefail

TREE=${1:?用法: install_into_tree.sh <TREE> <SAROOT>}
SA=${2:?用法: install_into_tree.sh <TREE> <SAROOT>}
HERE="$(cd "$(dirname "$0")" && pwd)"
DST="$TREE/vendor/ohemu/lutsa"
PRODUCT="$TREE/vendor/ohemu/qemu_x86_64_linux_full/config.json"

echo "== 1. 组件目录 =="
mkdir -p "$DST"/{src,include,idl,sa_profile,etc/init,sepolicy}
cp "$HERE/vendor/ohemu/lutsa/BUILD.gn"   "$DST/BUILD.gn"
cp "$HERE/vendor/ohemu/lutsa/bundle.json" "$DST/bundle.json"

echo "== 2. 业务内核源码 =="
cp "$SA/lut_sa.cpp" "$SA/session_workspace.cpp" "$SA/tile_compute.cpp" "$SA/tile_pool.cpp" "$DST/src/"
cp "$SA/../sched/qos_policy.cpp" "$DST/src/"
cp "$SA/lut_sa.h" "$SA/session_workspace.h" "$SA/tile_compute.h" "$SA/tile_pool.h" "$DST/include/"
cp "$SA/../sched/qos_policy.h" "$DST/include/"

echo "== 2b. LUT 内核：参考实现（算法自检锚点）+ 真 · 调优内核（按架构）=="
mkdir -p "$DST/include/t-mac"
cp "$SA/ref_kernel/lut_kernel_ref.h" "$SA/ref_kernel/lut_kernel_ref.cpp" "$DST/src/" 2>/dev/null || true
cp "$SA/ref_kernel/lut_kernel_ref.h" "$DST/include/"
cp "$SA/ref_kernel/lut_kernel_ref.cpp" "$DST/src/"
cp "$SA/ref_kernel/t-mac/tmac_gemm_wrapper.h" "$DST/include/t-mac/"

# 真内核来自仓库的 deploy/tuned/<目标>/（t-mac 引擎对各自硬件调优出的 kernels.cc）
REPO_ROOT="$(cd "$SA/../.." && pwd)"
TUNED_X64="$REPO_ROOT/deploy/tuned/ohos-x64-bitnet-3b"
TUNED_ARM="$REPO_ROOT/deploy/tuned/aarch64-hf-bitnet-3b"
if [ -f "$TUNED_X64/kernels.cc" ]; then
  cp "$TUNED_X64/kernels.cc" "$DST/src/kernels_tuned.cpp"
  cp "$TUNED_X64/kernels.h"  "$DST/include/t-mac/kernels.h"
  echo "  x86_64 调优内核: $(wc -l < "$TUNED_X64/kernels.cc") 行 → src/kernels_tuned.cpp"
else
  echo "  ⚠ 找不到 $TUNED_X64/kernels.cc（x86_64 调优内核）"
fi
if [ -f "$TUNED_ARM/kernels.cc" ]; then
  # arm64 版头的路径改成 kernels_arm64.h，避免与 x64 版同名冲突（BUILD.gn 按 target_cpu 选）
  sed 's|#include "t-mac/kernels.h"|#include "t-mac/kernels_arm64.h"|' "$TUNED_ARM/kernels.cc" > "$DST/src/kernels_tuned_arm64.cpp"
  cp "$TUNED_ARM/kernels.h" "$DST/include/t-mac/kernels_arm64.h"
  echo "  arm64 调优内核: $(wc -l < "$TUNED_ARM/kernels.cc") 行 → src/kernels_tuned_arm64.cpp"
fi

echo "== 3. SA 绑定 + 客户端 + IDL 生成物 =="
sed 's|#include "../lut_sa.h"|#include "lut_sa.h"|' "$SA/component/lut_sa_ability.cpp" > "$DST/src/lut_sa_ability.cpp"
cp "$SA/component/lut_sa_ability.h" "$DST/include/"
cp "$SA/component/lut_sa_client.cpp" "$DST/src/"     # 客户端：跨 IPC 调 6901，自证可用
cp "$SA/component/idl/"* "$DST/idl/"

echo "== 4. profile / init cfg / 取证服务 / sepolicy =="
cp "$SA/component/sa_profile/lut_sa.json" "$DST/sa_profile/"
cp "$SA/component/etc/init/lut_sa.cfg"    "$DST/etc/init/"
# 取证通道：串口一次性服务（本环境 hidumper 自身 exit=3、hdc 的 hdcd 被 watchdog 杀）
mkdir -p "$DST/evidence"
cp "$SA/component/etc/init/lut_evidence.cfg" "$DST/etc/init/"
cp "$SA/component/evidence/lut_evidence.sh"  "$DST/evidence/"
if [ -f "$SA/component/sepolicy/base/te/lut_sa.te" ]; then
  cp "$SA/component/sepolicy/base/te/lut_sa.te" "$DST/sepolicy/"
fi

echo "== 4b. SELinux 策略（关键：必须落到策略编译路径里，否则 SA 对客户端不可见）=="
SELINUX_DIR="$TREE/base/security/selinux_adapter/sepolicy"
mkdir -p "$SELINUX_DIR/ohos_policy/lutsa/lutsa_sa/public" "$SELINUX_DIR/ohos_policy/lutsa/lutsa_sa/system"
cp "$SA/component/sepolicy/ohos_policy/lutsa/lutsa_sa/public/lut_sa_service.te" \
   "$SELINUX_DIR/ohos_policy/lutsa/lutsa_sa/public/"
cp "$SA/component/sepolicy/ohos_policy/lutsa/lutsa_sa/system/lut_sa.te" \
   "$SELINUX_DIR/ohos_policy/lutsa/lutsa_sa/system/"
# service_contexts: SA_ID -> 类型；没有这一行，ServiceChecker 查不到 6901，
# 任何客户端 GetSystemAbility(6901) 都会被判 ERR_PERMISSION_DENIED
if ! grep -qE '^6901[[:space:]]' "$SELINUX_DIR/base/public/service_contexts"; then
  printf '6901\t\t\t\t   u:object_r:sa_lut_sa_service:s0\n' >> "$SELINUX_DIR/base/public/service_contexts"
  echo "  已追加 6901 → sa_lut_sa_service 到 service_contexts"
else
  echo "  service_contexts 里已有 6901"
fi

echo "== 5. 挂入产品配置 =="
python3 "$HERE/patch_product.py" "$PRODUCT"

echo "== 6. 校验 =="
ls -R "$DST" | head -30
grep -c '"subsystem"' "$PRODUCT" | xargs echo "  产品配置子系统条目:"
python3 -c "import json,sys;d=json.load(open('$PRODUCT'));print('  lutsa 已挂载:', any(c.get('component')=='lutsa' for s in d['subsystems'] for c in s.get('components',[])))"
