#!/bin/bash
set -e
cd /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main || exit 1
CC=/home/qwe123/miniconda3/envs/tvm-build/bin/x86_64-conda-linux-gnu-g++
INC=/mnt/d/ohos-models/x64inc

echo "=== 1. backup + fix CBits type in ags64 kernels.cc ==="
cp deploy/tuned/ohos-x64-ags64/kernels.cc deploy/tuned/ohos-x64-ags64/kernels.cc.bak
# x86 impl writes float32; buffer must be sized for float, not half.
sed -i 's/alignas(32) half CBits\[\([0-9]*\)\]/alignas(32) float CBits[\1]/g' deploy/tuned/ohos-x64-ags64/kernels.cc
echo "CBits declarations now:"
grep -n "CBits\[" deploy/tuned/ohos-x64-ags64/kernels.cc | grep -v "cse_var\|CBits\[cse" | head -8

echo ""
echo "=== 2. also check C_global decl ==="
grep -n "C_global\[[0-9]" deploy/tuned/ohos-x64-ags64/kernels.cc | head -5

echo ""
echo "=== 3. recompile kernels_gcc.o ==="
cp ohos/build/kernels_gcc.o ohos/build/kernels_gcc.o.bak
$CC -O2 -mavx2 -mf16c -c -I $INC deploy/tuned/ohos-x64-ags64/kernels.cc -o ohos/build/kernels_gcc.o
ls -la ohos/build/kernels_gcc.o

echo ""
echo "=== 4. rebuild + run test_full (musl) ==="
/home/qwe123/ohos-linux-sdk/native/llvm/bin/clang++ --target=x86_64-linux-ohos \
  --sysroot=/home/qwe123/ohos-linux-sdk/native/sysroot -static -O2 -I $INC \
  ohos/selftest/test_full.cpp ohos/build/kernels_gcc.o -o /tmp/test_full_musl2
/tmp/test_full_musl2 || echo "EXIT=$?"

echo ""
echo "=== 5. glibc reference ==="
$CC -O2 -I $INC ohos/selftest/test_full.cpp ohos/build/kernels_gcc.o -o /tmp/test_full_glibc2
/tmp/test_full_glibc2 || echo "EXIT=$?"
