#!/bin/bash
cd /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main || exit 1

echo "=== 1. staging kernels.cc status ==="
grep -n "half CBits\[\|float CBits\[\|uint64_t temp_CBits" ohos/staging-x64/t-mac/lib/kernels.cc | head -8

echo ""
echo "=== 2. is staging same as ags64? ==="
if diff -q deploy/tuned/ohos-x64-ags64/kernels.cc ohos/staging-x64/t-mac/lib/kernels.cc > /dev/null 2>&1; then
  echo "SAME (but ags64 was just fixed, staging is old)"
else
  echo "DIFF"
  echo "--- staging CBits lines:"
  grep -c "half CBits\[" ohos/staging-x64/t-mac/lib/kernels.cc
fi

echo ""
echo "=== 3. sync fixed version to staging ==="
cp deploy/tuned/ohos-x64-ags64/kernels.cc ohos/staging-x64/t-mac/lib/kernels.cc
echo "staging now has: $(grep -c 'float CBits\[' ohos/staging-x64/t-mac/lib/kernels.cc) float CBits declarations"

echo ""
echo "=== 4. check other staging files for half CBits ==="
grep -rn "half CBits\[" ohos/staging-x64/ 2>/dev/null | head -5
echo "(empty = clean)"

echo ""
echo "=== 5. verify llama.cpp integration builds use which kernels ==="
ls -la ohos/staging-x64/t-mac/lib/ 2>/dev/null | head -12
