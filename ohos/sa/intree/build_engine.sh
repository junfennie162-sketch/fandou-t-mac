#!/usr/bin/env bash
# build_engine.sh —— 用 OH 源码树**自己的** clang 把推理引擎编成静态库
#
# 为什么不在 GN 里编（STA-3 实测结论）：
#   ① OH 标准系统的 GN 工具链对所有 C++ 目标强制 -fno-exceptions，而这份 llama.cpp 快照
#      用 `throw std::runtime_error` 报错（110 处）→ 在 GN 里必然编不过；GN 模板又不暴露
#      configs（`configs += [...]` 报 "Undefined identifier"），拿不到 exceptions 开关。
#   ② 仓库里那对预编译 libllama.a/libggml.a 是 DevEco SDK 工具链编的，引用 std::__n1::*，
#      而 OH 源码树 libc++ 是 std::__h::* → ABI 命名空间不同，链接必然 undefined。
#   → 结论：同一份源码 + **OH 树自己的 clang** + 我们可控的 flags，编成 .a；GN 只负责链接它。
#
# 用法: build_engine.sh <OH源码树根> <LUT-SA组件目录(树内)>
set -euo pipefail

TREE=${1:?用法: build_engine.sh <TREE> <DST>}
DST=${2:?用法: build_engine.sh <TREE> <DST>}
ARCH=${LUTSA_ARCH:-x86_64}                 # x86_64 | arm64
L="$DST/llama"
OUT="$DST/prebuilt/libllama_engine.a"
OBJ="$DST/prebuilt/obj.$ARCH"

LLVM="$TREE/prebuilts/clang/ohos/linux-x86_64/llvm"
CLANG="$LLVM/bin/clang"
CLANGCXX="$LLVM/bin/clang++"
AR="$LLVM/bin/llvm-ar"
SYSROOT="$TREE/out/${ARCH}_virt/obj/third_party/musl"
[ -d "$SYSROOT" ] || SYSROOT=$(find "$TREE/out" -maxdepth 4 -type d -name musl -path "*obj/third_party*" 2>/dev/null | head -1)

if [ ! -x "$CLANGCXX" ]; then
  echo "❌ 找不到 OH 的 clang：$CLANGCXX"; exit 1
fi
if [ -z "${SYSROOT:-}" ] || [ ! -d "$SYSROOT" ]; then
  echo "❌ 找不到 musl sysroot（先跑过一次产品构建）"; exit 1
fi
echo "== 引擎编译（$ARCH）=="
echo "   clang : $($CLANGCXX --version | head -1)"
echo "   sysroot: $SYSROOT"

case "$ARCH" in
  x86_64) TARGET=x86_64-linux-ohos; SIMD="-mavx2 -mf16c";;
  arm64)  TARGET=aarch64-linux-ohos; SIMD="";;
  *) echo "❌ 未知 ARCH=$ARCH"; exit 1;;
esac

# 增量：源码/内核/壳/本脚本（flags 也在脚本里！）都没变就跳过
# （引擎 14 个 TU，含 24k 行的 ggml.c，重编要好几分钟）
HASH=$( { cat "$L"/src/*.cpp "$L"/src/*.h "$L"/ggml/src/*.c "$L"/ggml/src/*.cpp \
             "$L"/tmac/lib/kernels.cc "$L"/engine/*.cc "$0"; } 2>/dev/null | md5sum | cut -d' ' -f1)
if [ -f "$OUT" ] && [ -f "$DST/prebuilt/.engine.hash" ] \
   && [ "$HASH" = "$(cat "$DST/prebuilt/.engine.hash")" ] && [ "${LUTSA_FORCE_ENGINE:-0}" != "1" ]; then
  echo "  引擎源码未变（$HASH）→ 跳过重编（要强制重编：LUTSA_FORCE_ENGINE=1）"
  exit 0
fi

# 公共 flags：对齐 OH 工具链（-O2 -DNDEBUG -fPIC + musl 宏）与 DevEco 那次成功构建的
# compile_commands.json（-DGGML_USE_TMAC 等）。唯一不同：**保留异常**（llama.cpp 要用）。
COMMON="-target $TARGET --sysroot=$SYSROOT -O2 -DNDEBUG -fPIC -fno-omit-frame-pointer -funwind-tables"
COMMON="$COMMON -D__MUSL__ -D_LIBCPP_HAS_MUSL_LIBC -D_XOPEN_SOURCE=600 -DNDEBUG"
COMMON="$COMMON -DGGML_BUILD -DGGML_SHARED -DLLAMA_BUILD -DLLAMA_SHARED"
COMMON="$COMMON -DGGML_USE_LLAMAFILE -DGGML_USE_TMAC -DGGML_SCHED_MAX_COPIES=4"
COMMON="$COMMON -DTMAC_KCFG_FILE=\\\"/system/etc/lut_sa/kcfg.ini\\\""
COMMON="$COMMON -I$L/include -I$L/ggml/include -I$L/ggml/src -I$L/tmac/include"
COMMON="$COMMON -fvisibility=hidden -fvisibility-inlines-hidden $SIMD"
# 第三方源码：警告照打但不致命（不同 clang 小版本对 llama.cpp 的告警面不同）
WARN="-Wall -Wno-error -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable"

mkdir -p "$OBJ"
rm -f "$OBJ"/*.o

CC_FILES="llama/ggml/src/ggml.c llama/ggml/src/ggml-alloc.c llama/ggml/src/ggml-aarch64.c llama/ggml/src/ggml-quants.c"
CXX_FILES="llama/src/llama.cpp llama/src/llama-grammar.cpp llama/src/llama-sampling.cpp llama/src/llama-vocab.cpp llama/src/unicode.cpp llama/src/unicode-data.cpp llama/ggml/src/ggml-backend.cpp llama/ggml/src/ggml-tmac.cpp llama/ggml/src/llamafile/sgemm.cpp llama/engine/engine_shim.cc llama/engine/gguf_admission.cc"
if [ "$ARCH" = "arm64" ]; then
  CXX_FILES="$CXX_FILES llama/tmac/lib/kernels_arm64.cc"
else
  CXX_FILES="$CXX_FILES llama/tmac/lib/kernels.cc"
fi

FAIL=0
for f in $CC_FILES; do
  o="$OBJ/$(echo "$f" | tr '/' '_').o"
  echo "  [C ] $f"
  # -fexceptions 对 C 也加上：clang 在 C 模式下默认把函数标成 nounwind，
  # 于是从 C++ 侧（ggml-tmac.cpp）抛出的异常一旦要穿过 ggml.c 的栈帧就会 std::terminate
  # （实测：t-mac 模型缺 kcfg 时报 "libc++abi: terminating due to uncaught exception"，
  #   加了这个之后才被 engine_shim 的 catch 接住，变成"加载失败"而不是进程死亡）
  $CLANG $COMMON $WARN -std=gnu11 -fexceptions -c "$DST/$f" -o "$o" || FAIL=1
done
for f in $CXX_FILES; do
  o="$OBJ/$(echo "$f" | tr '/' '_').o"
  echo "  [C++ ] $f"
  # -fexceptions 是这一层能编通的关键；-frtti 留给可能用到 typeid 的第三方代码
  $CLANGCXX $COMMON $WARN -std=gnu++17 -fexceptions -frtti -c "$DST/$f" -o "$o" || FAIL=1
done
if [ "$FAIL" != "0" ]; then
  echo "❌ 有源文件编译失败"; exit 1
fi

rm -f "$OUT"
$AR rcs "$OUT" "$OBJ"/*.o
echo "$HASH" > "$DST/prebuilt/.engine.hash"
echo "  ✅ $OUT $(stat -c %s "$OUT") 字节 / $(ls "$OBJ"/*.o | wc -l) 个对象"
echo "  LUT 内核符号: $(nm --defined-only "$OUT" 2>/dev/null | grep -c 'qgemm_lut_t1_int8') 个 qgemm_lut_t1_int8_*"
echo "  引擎入口: $(nm --defined-only "$OUT" 2>/dev/null | grep -c 'lut_engine_') 个 lut_engine_*"
echo "  libc++ ABI: $(nm --undefined-only "$OUT" 2>/dev/null | grep -oE '_ZNSt[0-9A-Za-z_]+basic_string' | head -1 | grep -oE 'St[0-9A-Za-z_]+' | head -1)（应与 OH libc++ 的 __h 一致）"
