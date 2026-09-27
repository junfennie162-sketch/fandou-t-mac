#!/bin/bash
cd /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main || exit 1

echo "=== 1. ags64 kernels.cc: float_type / impl output ==="
grep -n "float_type\|_mm256_storeu_ps\|cvtps_ph" deploy/tuned/ohos-x64-ags64/kernels.cc | head -12

echo ""
echo "=== 2. bitnet-3b kernels.cc: float_type / impl output ==="
grep -n "float_type\|_mm256_storeu_ps\|cvtps_ph" deploy/tuned/ohos-x64-bitnet-3b/kernels.cc | head -12

echo ""
echo "=== 3. CBits declarations in ags64 ==="
grep -n "CBits\[\|C_global\[\|half Cadjust" deploy/tuned/ohos-x64-ags64/kernels.cc | head -20

echo ""
echo "=== 4. CBits declarations in bitnet-3b ==="
grep -n "CBits\[\|C_global\[\|half Cadjust" deploy/tuned/ohos-x64-bitnet-3b/kernels.cc | head -20

echo ""
echo "=== 5. which one compiled? check m256_k8640 CBits in bitnet-3b (Sep25 working) ==="
sed -n '1020,1060p' deploy/tuned/ohos-x64-bitnet-3b/kernels.cc | grep -E "CBits|tbl_|for " | head -10
