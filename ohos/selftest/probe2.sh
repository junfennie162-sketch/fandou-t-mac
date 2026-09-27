#!/bin/bash
cd /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main || exit 1
OD=/home/qwe123/ohos-linux-sdk/native/llvm/bin/llvm-objdump

echo "=== 1. qgemm macro/def in ags64 kernels.cc ==="
grep -n "qgemm" deploy/tuned/ohos-x64-ags64/kernels.cc | head -25

echo ""
echo "=== 2. buffer allocation lines (ags64) ==="
grep -n -E "float |int32_t |alignas|alloca|__attribute__|buf\[|tmp\[|resize" deploy/tuned/ohos-x64-ags64/kernels.cc | grep -iE "qgemm|buf|tmp|alloca" | head -20

echo ""
echo "=== 3. stack alloc per qgemm (objdump full + awk) ==="
$OD -d /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main/ohos/build/kernels_gcc.o > /tmp/kg.s 2>/dev/null
awk '/^[0-9a-f]+ <qgemm_lut_t1_int8_m/{name=$2} /subq \$0x[0-9a-f]+, %rsp|leaq -0x[0-9a-f]+\(%rbp\), %r1[0-9]/{if(name!=""){print name, $0; name=""}}' /tmp/kg.s | head -12

echo ""
echo "=== 4. which kernels.cc matches the compiled object? (diff sizes) ==="
for f in deploy/tuned/ohos-x64-ags64/kernels.cc deploy/tuned/ohos-x64-bitnet-3b/kernels.cc deploy/tuned/kernels.cc ohos/staging-x64/t-mac/lib/kernels.cc; do
  echo "-- $f: $(wc -l < $f) lines, $(wc -c < $f) bytes"
done

echo ""
echo "=== 5. qgemm body in ags64 (line 490-540 area) ==="
grep -n "qgemm_lut_t1_int8_m128_k3200" deploy/tuned/ohos-x64-ags64/kernels.cc | head -5
