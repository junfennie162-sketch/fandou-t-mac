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

echo "== 2c. STA-3 引擎源码（llama.cpp + ggml + t-mac LUT 内核）=="
# 为什么不用预编译 .a：那对 .a 是 DevEco SDK 工具链编的，引用 std::__n1::*，
# 而 OH 源码树的 libc++ 是 std::__h::* → ABI 命名空间不同，链接必报 undefined。
# 这里改成**同一份源码用 OH 工具链重编**，参数照抄 DevEco 那次成功构建的 compile_commands.json。
LLSRC="$REPO_ROOT/3rdparty/llama.cpp"
# 引擎侧 t-mac 头（wrapper / INIReader / dmlc shim）来自 staging；
# 而 **LUT 内核 + 形状表** 必须用 deploy/tuned/<目标>/ —— 那份才是针对 bitnet-3b 生成的
# （staging 里那套 kfactor=16 是早期版本；deploy 是修正版 kfactor=8）。实测：用错那套会让
# ggml-tmac 在加载期报 "Failed to find kcfg"
if [ "$(uname -m)" = "aarch64" ]; then
  TMAC_INC="$REPO_ROOT/ohos/staging-arm64/t-mac/include"
  TMAC_KER="$REPO_ROOT/deploy/tuned/aarch64-hf-bitnet-3b"
else
  TMAC_INC="$REPO_ROOT/ohos/staging-x64/t-mac/include"
  # ★ LUT 内核 + 形状表用 staging-x64 这一对（kfactor=16 / lut_scales_size=135,50）：
  #   2026-10-05 实测（evidence/45）——同一份 bitnet-3b-tmac.gguf，用这对参数时
  #   logits 正常、Generate 出真实文本且可复现（失败项 0）；改用 deploy/tuned/ohos-x64-bitnet-3b
  #   那对（kfactor=8 / lut_scales_size=1）会**全 NaN**（evidence/44）。
  #   两者 key 名相同、只有参数不同，所以"看起来能查到"但数值全错 —— 这类问题只有跑数值才现形。
  TMAC_KER="$REPO_ROOT/ohos/staging-x64/t-mac/lib"
fi
# 实验旋钮：临时换一套 LUT 内核/形状表（比如试 staging 那份 kfactor=16 的）
#   TMAC_KER_OVERRIDE=<dir with kernels.cc + kcfg.ini + kernels.h>
if [ -n "${TMAC_KER_OVERRIDE:-}" ]; then
  TMAC_KER="$TMAC_KER_OVERRIDE"
  echo "  ⚠ 用 TMAC_KER_OVERRIDE 指定的内核目录: $TMAC_KER"
fi
L=$DST/llama
rm -rf "$L"
mkdir -p "$L/src" "$L/include" "$L/ggml/include" "$L/ggml/src/llamafile" \
         "$L/tmac/include/t-mac" "$L/tmac/include/dmlc" "$L/tmac/lib" \
         "$DST/etc/lut_sa"
cp "$LLSRC"/src/*.cpp "$LLSRC"/src/*.h "$L/src/"
cp "$LLSRC/include/llama.h" "$L/include/"
cp "$LLSRC"/ggml/include/*.h "$L/ggml/include/"
ls "$L/ggml/include/" > /dev/null
# ggml 源只取顶层 + llamafile/sgemm（cuda/vulkan/metal/cann… 这些平台专项目录不拷）
cp "$LLSRC"/ggml/src/*.c "$LLSRC"/ggml/src/*.cpp "$LLSRC"/ggml/src/*.h "$L/ggml/src/" 2>/dev/null
cp "$LLSRC"/ggml/src/llamafile/sgemm.cpp "$LLSRC"/ggml/src/llamafile/sgemm.h "$L/ggml/src/llamafile/" 2>/dev/null
echo "  llama 源码: $(ls "$L/src" | wc -l) 个 / ggml 源码: $(ls "$L/ggml/src" | wc -l) 个"

if [ -d "$TMAC_INC" ]; then
  cp -r "$TMAC_INC/." "$L/tmac/include/"
  echo "  t-mac 头: $(ls "$L/tmac/include/t-mac" | tr '\n' ' ')"
else
  echo "  ⚠ 缺 $TMAC_INC（t-mac 引擎头）"
fi
# LOG(FATAL) 在 shim 里默认 abort()：SA 是系统能力，一份形状对不上的模型不能把进程打死。
# 改成抛异常 → 引擎壳的 catch 把它变成"加载失败+原因"，SA 继续活着（STA-3 实测踩过：
# bitnet-3b-tmac.gguf 的张量形状与可用 LUT 内核不匹配 → "Failed to find kcfg" → abort）
python3 - "$L/tmac/include/dmlc/logging.h" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
orig = s
# 注意：这里**逐条**替换，不要用"整块只在首次生效"的守卫 —— 踩过：加了新替换项后
# 因为文件里已有旧标记，整块被跳过，新替换项永远不生效（DLOG 那条就这么被吞了一轮）
subs = [
    # FATAL 不再 abort：系统能力里不能让第三方库的 FATAL 打死进程 → 抛异常，
    # 由 engine_shim 的 catch 转成"加载失败 + 原因"，SA 继续服务
    ('      std::abort();',
     '      // TMAC_SA_GRACEFUL_FATAL：抛异常而不是 abort（由引擎壳 catch）\n'
     '      throw std::runtime_error("dmlc LOG(FATAL): " + stream_.str());'),
    ('#include <sstream>',
     '#include <sstream>\n#include <stdexcept>  // TMAC_SA_GRACEFUL_FATAL'),
    # WARNING / DLOG 从 NullStream 改为可见：ggml-tmac 在 transform 前会 DLOG 出
    # "正在转换哪个张量 + 查到的 kcfg"，是定位"哪个张量查表失败"的唯一线索
    ('#define LOG_WARNING ::dmlc::shim_detail::NullStream()',
     '#define LOG_WARNING ::dmlc::shim_detail::LogMessage(__FILE__, __LINE__, false)'),
    ('#define DLOG(severity) LOG_INFO',
     '#define DLOG(severity) ::dmlc::shim_detail::LogMessage(__FILE__, __LINE__, false)'),
    # ★ 关键：shim 的 LogMessage 析构函数**只在 fatal_ 时才输出**，所以上面把 WARNING/DLOG
    #   映射成 LogMessage 也没用（消息照样被吞）。改成一律打印，WARNING/DLOG 才真的可见
    ('  ~LogMessage() {\n    if (fatal_) {\n      std::cerr << stream_.str() << std::endl;',
     '  ~LogMessage() {\n    std::cerr << stream_.str() << std::endl;  // TMAC_SA_VISIBLE\n'
     '    if (fatal_) {'),
]
applied = []
for old, new in subs:
    if old in s:
        s = s.replace(old, new)
        applied.append(old.strip()[:40])
if s != orig:
    open(p, 'w', encoding='utf-8').write(s)
    print('  dmlc shim 已改 %d 处：%s' % (len(applied), ' / '.join(applied)))
else:
    print('  dmlc shim 已是目标状态（FATAL→抛异常、WARNING/DLOG 可见）')
PY
if [ -f "$TMAC_KER/kernels.cc" ]; then
  cp "$TMAC_KER/kernels.cc" "$L/tmac/lib/kernels.cc"
  cp "$TMAC_KER/kcfg.ini"   "$DST/etc/lut_sa/kcfg.ini"
  echo "  LUT 内核 kernels.cc $(wc -l < "$L/tmac/lib/kernels.cc") 行 + kcfg.ini $(wc -l < "$DST/etc/lut_sa/kcfg.ini") 行（来自 $TMAC_KER）"
  echo "  kernels.cc 覆盖的形状: $(grep -oE 'qgemm_lut_t1_int8_m[0-9]+_k[0-9]+_n1_b2' "$L/tmac/lib/kernels.cc" | sort -u | tr '\n' ' ')"
  echo "  kcfg.ini 覆盖的形状: $(grep -oE 'qgemm_lut_t1_int8_m[0-9]+_k[0-9]+_n1_b2' "$DST/etc/lut_sa/kcfg.ini" | sort -u | tr '\n' ' ')"
else
  echo "  ⚠ 缺 $TMAC_KER/kernels.cc（LUT 内核）"
fi
# arm64 变体（真机路径）：NEON 内核 + 把 include 改到 kernels_arm64.h，避免与 x64 版同名冲突
if [ -f "$REPO_ROOT/deploy/tuned/aarch64-hf-bitnet-3b/kernels.cc" ]; then
  sed 's|#include "t-mac/kernels.h"|#include "t-mac/kernels_arm64.h"|' \
      "$REPO_ROOT/deploy/tuned/aarch64-hf-bitnet-3b/kernels.cc" > "$L/tmac/lib/kernels_arm64.cc"
  cp "$REPO_ROOT/deploy/tuned/aarch64-hf-bitnet-3b/kernels.h" "$L/tmac/include/t-mac/kernels_arm64.h"
  echo "  arm64 内核: $(wc -l < "$L/tmac/lib/kernels_arm64.cc") 行 → kernels_arm64.cc"
else
  echo "  （无 arm64 调优内核：本次只出 x86_64，真机路径见 QEMU-DEPLOY.md）"
fi
# 业务层的 kernels.h 与引擎那份必须逐字节一致（inline 分派器同名，否则 ODR 打架）
if [ -f "$TMAC_KER/kernels.h" ]; then
  cp "$TMAC_KER/kernels.h" "$DST/include/t-mac/kernels.h"
  cmp -s "$TMAC_KER/kernels.h" "$L/tmac/include/t-mac/kernels.h" \
    && echo "  业务层/引擎 kernels.h: 一致 ✓" \
    || echo "  ⚠ 业务层与引擎的 kernels.h 不一致"
fi

# 引擎壳（C 接口，业务层只看它）：头进 include/（业务层 -I 就这一处），实现进 llama/engine/ 编进 .a
mkdir -p "$L/engine"
cp "$SA/engine/engine_shim.h" "$SA/engine/engine_shim.cc" "$SA/engine/gguf_admission.cc" "$L/engine/" 2>/dev/null
cp "$SA/engine/engine_shim.h" "$DST/include/" 2>/dev/null \
  && echo "  引擎壳: engine_shim.h → include/（业务层接口）, engine_shim.cc + gguf_admission.cc → llama/engine/（编进 .a）"

echo "== 2d. 编引擎静态库（OH 树自己的 clang，带异常）=="
bash "$HERE/build_engine.sh" "$TREE" "$DST" 2>&1 | tail -12

echo "== 3. SA 绑定 + 客户端 + IDL 生成物 =="
sed 's|#include "../lut_sa.h"|#include "lut_sa.h"|' "$SA/component/lut_sa_ability.cpp" > "$DST/src/lut_sa_ability.cpp"
cp "$SA/component/lut_sa_ability.h" "$DST/include/"
cp "$SA/component/lut_sa_client.cpp" "$DST/src/"     # 客户端：跨 IPC 调 6901，自证可用
cp "$SA/component/lut_a11y_dump.cpp" "$DST/src/"     # S7-1a：无障碍读屏探针（独立工具）
cp "$SA/component/idl/"* "$DST/idl/"

# S7-1a 链接期：探针要链镜像里现成的无障碍库（两个，缺一不可：
#   libaccessibleability.z.so    = 客户端（AccessibleAbilityClient）
#   libaccessibility_common.z.so = 元素结构体实现（AccessibilityElementInfo::GetContent 等）
# 不走 external_deps（那会拖 runtime_core:ani + napi 进构建闭包），
# 改成把镜像里那两份拷进 prebuilt/ —— 每轮 install 都刷新，不会与镜像版本漂移。
mkdir -p "$DST/prebuilt"
A11Y_LIBS=""
for pair in "libaccessibleability.z.so:lib64" "libaccessibility_common.z.so:lib64/platformsdk"; do
  n=${pair%%:*}; d=${pair#*:}
  for base in "$TREE/out/x86_64_virt/packages/phone/system" "$TREE/out/x86_64_virt"; do
    if [ -f "$base/$d/$n" ]; then
      cp -f "$base/$d/$n" "$DST/prebuilt/$n"
      A11Y_LIBS="$A11Y_LIBS $n"
      break
    fi
  done
done
if [ -n "$A11Y_LIBS" ]; then
  echo "  无障碍库 → prebuilt/：$A11Y_LIBS"
else
  echo "  ⚠ 没找到无障碍客户端库（先整体编过一遍镜像？）—— 探针链接会失败"
fi

echo "== 4. profile / init cfg / 取证服务 / sepolicy =="
cp "$SA/component/sa_profile/lut_sa.json" "$DST/sa_profile/"
cp "$SA/component/etc/init/lut_sa.cfg"    "$DST/etc/init/"
# 取证通道：串口一次性服务（本环境 hidumper 自身 exit=3、hdc 的 hdcd 被 watchdog 杀）
mkdir -p "$DST/evidence"
cp "$SA/component/etc/init/lut_evidence.cfg"  "$DST/etc/init/"
cp "$SA/component/evidence/lut_evidence.sh"  "$DST/evidence/"
cp "$SA/component/etc/init/lut_evidence2.cfg" "$DST/etc/init/"        # 重的分段单独一个服务（FIX-71）
cp "$SA/component/evidence/lut_evidence2.sh" "$DST/evidence/"
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
