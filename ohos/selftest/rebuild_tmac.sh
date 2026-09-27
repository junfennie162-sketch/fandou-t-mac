#!/bin/bash
cd /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main/3rdparty/llama.cpp/build-ohos-x86-tmac || exit 1

echo "=== 1. TMAC / compiler config from CMakeCache ==="
grep -iE "tmac|kernels|CMAKE_CXX_COMPILER:|CMAKE_C_COMPILER:|CMAKE_MAKE_PROGRAM" CMakeCache.txt | head -15

echo ""
echo "=== 2. build system ==="
ls -la build.ninja Makefile 2>/dev/null | head -4

echo ""
echo "=== 3. staging kernels.cc mtime (should be 19:24 fixed) ==="
ls -la /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main/ohos/staging-x64/t-mac/lib/kernels.cc

echo ""
echo "=== 4. rebuild ==="
if [ -f build.ninja ]; then
  ninja -j6 2>&1 | tail -12
else
  make -j6 2>&1 | tail -12
fi

echo ""
echo "=== 5. outputs ==="
ls -la bin/ 2>/dev/null | head -10
find . -maxdepth 2 -name "llama-cli" -o -maxdepth 2 -name "libggml*.so" -o -maxdepth 2 -name "libllama*.so" 2>/dev/null | head -10
