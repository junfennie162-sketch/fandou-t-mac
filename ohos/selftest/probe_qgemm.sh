#!/bin/bash
# Probe qgemm stack allocations and find where qgemm comes from.
cd /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main/ohos/build || exit 1
OD=/home/qwe123/ohos-linux-sdk/native/llvm/bin/llvm-objdump

echo "=== 1. stack allocation per qgemm kernel (kernels_gcc.o) ==="
for f in qgemm_lut_t1_int8_m128_k3200_n1_b2 qgemm_lut_t1_int8_m256_k3200_n1_b2 qgemm_lut_t1_int8_m256_k8640_n1_b2 qgemm_lut_t1_int8_m6400_k3200_n1_b2 qgemm_lut_t1_int8_m6400_k8640_n1_b2 qgemm_lut_t1_int8_m17280_k3200_n1_b2; do
  echo "== $f =="
  $OD -d --disassemble-symbols=$f kernels_gcc.o 2>/dev/null | grep -E "subq .*%rsp|leaq .*-0x[0-9a-f]+\(%rbp\), %r1[0-9]" | head -3
done

echo "=== 2. does repo kernels.cc contain qgemm? ==="
grep -c "qgemm" /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main/deploy/tuned/kernels.cc

echo "=== 3. all kernels.cc copies on disk ==="
find /mnt/c/Users/NJF/Desktop/t-mac /mnt/d/ohos-models /home/qwe123 /tmp -maxdepth 6 -name "kernels.cc" 2>/dev/null | head -10

echo "=== 4. qgemm generator in python/ ==="
grep -rln "qgemm_lut" /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main/python /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main/deploy 2>/dev/null | head -10
