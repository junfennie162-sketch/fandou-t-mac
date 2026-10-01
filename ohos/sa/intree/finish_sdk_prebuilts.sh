#!/usr/bin/env bash
# finish_sdk_prebuilts.sh —— 把已编好的 SDK 产物手工后处理成 prebuilts/ohos-sdk/linux/<api>/
#
# 为什么需要它：
#   OH 7.0-Release 的 SDK 构建里有一个残缺目标 —— cortex-m 的 NDK libc
#   （interface/sdk_c/third_party/musl/ndk_script/adapter 的 libc_ndk_lite）：
#       ld.lld: error: undefined symbol: __aeabi_unwind_cpp_pr0
#   该目标链不到 libunwind（rsp 里根本没带），导致 `build_ohos_sdk` 整体失败；
#   失败后 hb 不会执行 _post_process_sdk，于是 prebuilts/ohos-sdk 始终是空的，
#   产品构建随即在 webview 处报：
#       'prebuilts/ohos-sdk/linux/26.0.0/.../libbundle_ndk.z.so', missing and no known rule
#
# 做法：用 `ninja -w dupbuild=warn -k 0 ... build_ohos_sdk` 把 SDK 里**除该目标外**的部分编完，
#       再按 hb 的 _post_process_sdk() 逻辑手工搬运产物。
#       一旦 prebuilts/ohos-sdk/linux/<api>/ 存在，build.sh 的 should_build_sdk() 就会返回 False
#       → 后续构建自动跳过 SDK 阶段（这是 OH 自己的判断条件，不是 hack）。
#
# 用法: finish_sdk_prebuilts.sh <TREE> [JOBS]
set -uo pipefail

TREE=${1:?用法: finish_sdk_prebuilts.sh <TREE> [JOBS]}
JOBS=${2:-10}
NINJA="$TREE/prebuilts/build-tools/linux-x86/bin/ninja"

API=$(grep -oE 'api_full_version[[:space:]]*=[[:space:]]*"[^"]+"' "$TREE/build/version.gni" | head -1 | sed -E 's/.*"([^"]+)".*/\1/')
[ -n "$API" ] || { echo "❌ 无法从 build/version.gni 取到 api_full_version"; exit 1; }
echo "== api_full_version = $API =="

echo "== 1. 续编 SDK（忽略失败继续，cortex-m 目标会 FAILED 但不影响其它产物）=="
"$NINJA" -w dupbuild=warn -k 0 -j "$JOBS" -C "$TREE/out/sdk" build_ohos_sdk \
  2>&1 | grep -vE 'multiple rules generate' | tail -20

echo
echo "== 2. 手工后处理（等价 PrebuiltSdk._post_process_sdk）=="
python3 - "$TREE" "$API" <<'PY'
import os, shutil, sys

tree, api = sys.argv[1], sys.argv[2]
sdk_pre = os.path.join(tree, "prebuilts", "ohos-sdk")

# 2.1 清掉旧的 linux 目录
old = os.path.join(sdk_pre, "linux")
if os.path.exists(old):
    shutil.rmtree(old)
os.makedirs(sdk_pre, exist_ok=True)

# 2.2 out/sdk/ohos-sdk/linux → prebuilts/ohos-sdk/linux
out_sdk = os.path.join(tree, "out", "sdk", "ohos-sdk", "linux")
if os.path.exists(out_sdk):
    shutil.move(out_sdk, sdk_pre)
    print("  ✓ out/sdk/ohos-sdk/linux → prebuilts/ohos-sdk/linux")

# 2.3 sdk-native/{os-irrelevant, os-specific/linux} → prebuilts/ohos-sdk/linux/native
native_target = os.path.join(sdk_pre, "linux", "native")
os.makedirs(native_target, exist_ok=True)
for nd in (os.path.join(tree, "out", "sdk", "sdk-native", "os-irrelevant"),
           os.path.join(tree, "out", "sdk", "sdk-native", "os-specific", "linux")):
    if not os.path.exists(nd):
        continue
    for item in os.listdir(nd):
        src, dst = os.path.join(nd, item), os.path.join(native_target, item)
        if os.path.exists(dst):
            shutil.rmtree(dst) if os.path.isdir(dst) else os.remove(dst)
        shutil.move(src, dst)
    print(f"  ✓ {os.path.relpath(nd, tree)} → linux/native")

# 2.4 linux/* → linux/<api>/*
linux_sdk = os.path.join(sdk_pre, "linux")
api_dir = os.path.join(linux_sdk, api)
os.makedirs(api_dir, exist_ok=True)
for item in os.listdir(linux_sdk):
    if item == api:
        continue
    p = os.path.join(linux_sdk, item)
    if os.path.isdir(p):
        shutil.move(p, os.path.join(api_dir, item))
        print(f"  ✓ linux/{item} → linux/{api}/{item}")

# 2.5 previewer 包（可选）
prev = os.path.join(linux_sdk, api, "previewer")
os.makedirs(prev, exist_ok=True)
src_pkg = os.path.join(linux_sdk, api, "native", "oh-uni-package.json")
if os.path.exists(src_pkg):
    c = open(src_pkg, encoding="utf-8").read().replace("Native", "Previewer").replace("native", "previewer")
    open(os.path.join(prev, "oh-uni-package.json"), "w", encoding="utf-8").write(c)
    print("  ✓ previewer/oh-uni-package.json")
PY

echo
echo "== 3. 校验：产品构建要的那个文件在不在了 =="
TARGET="$TREE/prebuilts/ohos-sdk/linux/$API/native/sysroot/usr/lib/x86_64-linux-ohos/libbundle_ndk.z.so"
if [ -f "$TARGET" ]; then
  echo "  ✅ $TARGET"
else
  echo "  ⚠ 仍缺 $TARGET —— 说明 x86_64 的 NDK 侧还没编出来，检查 out/sdk/sdk-native/os-irrelevant/sysroot/usr/lib/"
  ls "$TREE/out/sdk/sdk-native/os-irrelevant/sysroot/usr/lib/" 2>/dev/null
fi
echo
echo "完成后直接重跑： ./build.sh --product-name x86_64_virt --ccache --jobs $JOBS"
echo "（prebuilts/ohos-sdk/linux/$API 已存在 → SDK 阶段会被 should_build_sdk() 自动跳过）"
