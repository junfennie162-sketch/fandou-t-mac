#!/bin/bash
cd /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main || exit 1

echo "=== 1. ohos/ tree (sh scripts + cmake dirs) ==="
find ohos -maxdepth 3 \( -name "*.sh" -o -name "CMakeCache.txt" -o -name "*.ps1" \) 2>/dev/null | head -25

echo ""
echo "=== 2. llama.cpp build caches anywhere ==="
find /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main /mnt/d/ohos-models /tmp -maxdepth 4 -name "CMakeCache.txt" 2>/dev/null | head -10

echo ""
echo "=== 3. WSL ohos-models dir ==="
ls -la /mnt/d/ohos-models/ 2>/dev/null | head -25

echo ""
echo "=== 4. llama.cpp build dirs in repo ==="
find /mnt/c/Users/NJF/Desktop/t-mac/fandou-t-mac-main -maxdepth 4 -type d -name "build*" 2>/dev/null | grep -v 3rdparty | head -10

echo ""
echo "=== 5. existing device binaries in /data/local/tmp dirs (check hdc targets later) ==="
ls -la /tmp/llm* 2>/dev/null | head -10
