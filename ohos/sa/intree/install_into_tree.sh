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

echo "== 2b. 便携参考 LUT 内核（与引擎同接口：include/t-mac/tmac_gemm_wrapper.h） =="
mkdir -p "$DST/include/t-mac"
cp "$SA/ref_kernel/lut_kernel_ref.h" "$DST/include/"
cp "$SA/ref_kernel/lut_kernel_ref.cpp" "$DST/src/"
cp "$SA/ref_kernel/t-mac/tmac_gemm_wrapper.h" "$DST/include/t-mac/"

echo "== 3. SA 绑定 + IDL 生成物 =="
sed 's|#include "../lut_sa.h"|#include "lut_sa.h"|' "$SA/component/lut_sa_ability.cpp" > "$DST/src/lut_sa_ability.cpp"
cp "$SA/component/lut_sa_ability.h" "$DST/include/"
cp "$SA/component/idl/"* "$DST/idl/"

echo "== 4. profile / init cfg / sepolicy =="
cp "$SA/component/sa_profile/lut_sa.json" "$DST/sa_profile/"
cp "$SA/component/etc/init/lut_sa.cfg"    "$DST/etc/init/"
if [ -f "$SA/component/sepolicy/base/te/lut_sa.te" ]; then
  cp "$SA/component/sepolicy/base/te/lut_sa.te" "$DST/sepolicy/"
fi

echo "== 5. 挂入产品配置 =="
python3 "$HERE/patch_product.py" "$PRODUCT"

echo "== 6. 校验 =="
ls -R "$DST" | head -30
grep -c '"subsystem"' "$PRODUCT" | xargs echo "  产品配置子系统条目:"
python3 -c "import json,sys;d=json.load(open('$PRODUCT'));print('  lutsa 已挂载:', any(c.get('component')=='lutsa' for s in d['subsystems'] for c in s.get('components',[])))"
