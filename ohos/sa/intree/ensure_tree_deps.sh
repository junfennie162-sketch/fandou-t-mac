#!/usr/bin/env bash
# 补齐 OH 7.0 源码树中「产品层」缺失件（7.0 manifest 不含 vendor 仓，需手动补）：
#   1) vendor/ohemu              —— QEMU 产品配置 + 官方 qemu_run.sh
#   2) productdefine/common      —— 产品 inherit（rich.json / chipset_common.json）
#   3) subsystem_config.json     —— 注册 ohemu 子系统 → vendor/ohemu（否则构建报 subsystem not found）
# 幂等：可反复执行。
# 用法: ensure_tree_deps.sh <OH源码树根>
set -uo pipefail

TREE=${1:?用法: ensure_tree_deps.sh <TREE>}
TAG=OpenHarmony-v7.0-Release
GITCODE=https://gitcode.com/openharmony

need_git() {
  if [ ! -d "$TREE/$2" ]; then
    echo "  补克隆 $1 → $2"
    git clone -q --depth=1 -b "$TAG" "$GITCODE/$1.git" "$TREE/$2" || echo "  ❌ $1 克隆失败"
  else
    echo "  ✓ $2 已存在"
  fi
}

echo "[ensure] vendor/ohemu"
need_git vendor_ohemu vendor/ohemu
echo "[ensure] productdefine_common"
need_git productdefine_common productdefine

echo "[ensure] 注册 ohemu 子系统（build/subsystem_config.json）"
python3 - "$TREE" <<'PY'
import json
import os
import re
import sys

tree = sys.argv[1]
# OH 7.0：//build/subsystem_config.json 为「子系统名 → {path,name}」字典
path = os.path.join(tree, "build/subsystem_config.json")
if not os.path.isfile(path):
    print(f"  ⚠ 未找到 {path}")
    sys.exit(0)
data = json.loads(re.sub(r"//[^\n]*", "", open(path, encoding="utf-8").read()))
if "ohemu" in data:
    print(f"  ✓ 已注册: ohemu → {data['ohemu'].get('path')}")
else:
    data["ohemu"] = {"path": "vendor/ohemu", "name": "ohemu"}
    open(path, "w", encoding="utf-8").write(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
    print("  ✅ 已注册: ohemu → vendor/ohemu")
PY

echo "[ensure] 校验"
ls "$TREE/vendor/ohemu/qemu_x86_64_linux_full/config.json" >/dev/null 2>&1 && echo "  ✓ 产品配置就位"
ls "$TREE/productdefine/common/inherit/rich.json" >/dev/null 2>&1 && echo "  ✓ rich.json 就位"
