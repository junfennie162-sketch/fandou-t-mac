#!/usr/bin/env bash
# 补齐 OH 7.0 源码树中「产品层」缺失件，并应用 7.0 兼容修复（幂等，可反复执行）：
#   1) vendor/ohemu              —— QEMU 产品配置 + 官方 qemu_run.sh
#   2) productdefine/common      —— 产品 inherit（rich.json / chipset_common.json）
#   3) subsystem_config.json     —— 注册 ohemu → vendor/ohemu/lutsa（只扫组件目录）
#   4) 兼容修复 4 处              —— 见下方 FIX-1 ~ FIX-4
# 用法: ensure_tree_deps.sh <OH源码树根>
set -uo pipefail

TREE=${1:?用法: ensure_tree_deps.sh <TREE>}
TAG=OpenHarmony-v7.0-Release
GITCODE=https://gitcode.com/openharmony
PRODUCT="$TREE/vendor/ohemu/qemu_x86_64_linux_full/config.json"

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

echo "[ensure] 注册子系统（build/subsystem_config.json）"
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

# FIX-1：ohemu 的 path 必须指向「只含组件」的目录。
# 写成 vendor/ohemu 时，hb 会递归扫整个 vendor/ohemu，命中各产品自带的 bundle.json
# （声明 product_x86_64_virt / product_qemu_csky_* 等子系统名），触发
# "subsystem name config incorrect" 导致 LOAD 失败。
want = "vendor/ohemu/lutsa"
data.setdefault("ohemu", {})["path"] = want
data["ohemu"]["name"] = "ohemu"
print(f"  ✅ ohemu → {want}")

# 产品自带 bundle.json 声明子系统 product_x86_64_virt（power_config 部件），需一并注册
data.setdefault("product_x86_64_virt", {
    "path": "vendor/ohemu/qemu_x86_64_linux_full",
    "name": "product_x86_64_virt",
})
print("  ✅ product_x86_64_virt → vendor/ohemu/qemu_x86_64_linux_full")

open(path, "w", encoding="utf-8").write(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
PY

echo "[ensure] 兼容修复"
python3 - "$TREE" <<'PY'
import json
import os
import re
import subprocess
import sys

tree = sys.argv[1]
prod = os.path.join(tree, "vendor", "ohemu", "qemu_x86_64_linux_full")
product = os.path.join(prod, "config.json")

# FIX-2：删掉产品目录下的 subsystem_config_overlay.json。
# 7.0 hb 的 merge_subsystem_overlay() 对 no_src_subsystem 用
# subsystem_config_overlay.get('subsystem')，overlay 若无 subsystem 段即
# TypeError: 'NoneType' object is not subscriptable（上游 bug）。留着只会踩雷。
overlay = os.path.join(prod, "subsystem_config_overlay.json")
if os.path.isfile(overlay):
    os.remove(overlay)
    print("  ✅ 已删除误建的 subsystem_config_overlay.json")

# FIX-3：产品自带 bundle.json 不能改名——它是产品部件（power_config）的声明处。
disabled = os.path.join(prod, "bundle.json.disabled")
if os.path.isfile(disabled) and not os.path.isfile(os.path.join(prod, "bundle.json")):
    os.rename(disabled, os.path.join(prod, "bundle.json"))
    print("  ✅ 已恢复 bundle.json（原 bundle.json.disabled）")

# FIX-4：产品配置必须是「上游 subsystems + ohemu:lutsa」。
# 上游含 thirdparty(typescript/alsa-lib/…) 与 sdk 两个子系统：
#   少 sdk        → GN 报 "OHOS component : (sdk) not found"（ets2abc_config.gni:337）
#   少 thirdparty → 缺 typescript/alsa-lib，standard 系统编不出来
try:
    raw = subprocess.check_output(
        ["git", "-C", os.path.join(tree, "vendor", "ohemu"), "show",
         "HEAD:qemu_x86_64_linux_full/config.json"], text=True)
    upstream = json.loads(raw)
except Exception as e:
    print(f"  ⚠ 无法从 git 取上游 config.json（{e}），跳过还原")
    upstream = None

if upstream is not None and os.path.isfile(product):
    up_names = {s.get("subsystem") for s in upstream.get("subsystems", [])}
    cur = json.load(open(product, encoding="utf-8"))
    cur_names = {s.get("subsystem") for s in cur.get("subsystems", [])}
    missing = up_names - cur_names
    if missing:
        # 以上游为底，保留当前多出来的子系统（如 ohemu）
        merged = list(upstream.get("subsystems", []))
        extra = [s for s in cur.get("subsystems", []) if s.get("subsystem") not in up_names]
        merged.extend(extra)
        upstream["subsystems"] = merged
        open(product, "w", encoding="utf-8").write(
            json.dumps(upstream, ensure_ascii=False, indent=2) + "\n")
        print(f"  ✅ 还原缺失子系统 {sorted(missing)}，保留 {[s.get('subsystem') for s in extra]}")
    else:
        print("  ✓ 产品配置子系统完整（thirdparty / sdk 在位）")

# FIX-5：vendored vendor/ohemu 比 7.0 的 drivers_peripheral_audio 新，多带一个
# 7.0 驱动不认识的特性（全树无消费者），会在 LOAD 报 feature 2006，删掉该行。
for name in ("virt_common.json", "virt_common_x86_64.json"):
    p = os.path.join(tree, "vendor", "ohemu", "virt", name)
    if not os.path.isfile(p):
        continue
    s = open(p, encoding="utf-8").read()
    new = re.sub(r'\n[ \t]*"drivers_peripheral_audio_vendor_alsa_path[^\n]*",?', '', s)
    if new != s:
        new = re.sub(r',(\s*\])', r'\1', new)
        open(p, "w", encoding="utf-8").write(new)
        print(f"  ✅ 清理孤儿特性 drivers_peripheral_audio_vendor_alsa_path（{name}）")
PY

echo "[ensure] git-lfs（FIX-10）"
# repo sync --depth=1 不会拉 git-lfs 对象，树里会留下 100+ 字节的指针文件；
# arkui 的 idlizer-*.tgz 是硬需求（npm install 会因 TAR_BAD_ARCHIVE 直接失败），
# device/qemu、foundation/graphic、foundation/multimedia、applications/standard 等目录里也有。
# 这里只在检测到"仍是指针"时才拉，幂等。
if command -v git-lfs >/dev/null 2>&1; then
  LFS_LIST=$(cd "$TREE" && find . -path ./out -prune -o -type f -size -400c \
      \( -name '*.tgz' -o -name '*.zip' -o -name '*.gz' -o -name '*.so' -o -name '*.a' \
         -o -name '*.bin' -o -name '*.dat' -o -name '*.jar' -o -name '*.har' \) -print 2>/dev/null \
      | sed 's|^\./||' \
      | xargs -d '\n' grep -l "git-lfs.github.com/spec" 2>/dev/null \
      | grep -vE '^(test/xts|docs)/')
  if [ -z "$LFS_LIST" ]; then
    echo "  ✓ 无待补的 LFS 对象"
  else
    echo "  发现 $(echo "$LFS_LIST" | wc -l) 个 LFS 指针，按仓库补拉："
    echo "$LFS_LIST" | while IFS= read -r f; do
      d=$(dirname "$TREE/$f")
      while [ "$d" != "$TREE" ] && [ ! -e "$d/.git" ]; do d=$(dirname "$d"); done
      [ -e "$d/.git" ] || continue
      rel="${f#${d#$TREE/}/}"
      case "$rel" in /*) rel="${rel#/}";; esac
      pat="$(dirname "$rel")/*.$(basename "$rel" | sed 's/.*\.//')"
      git -C "$d" lfs pull --include="$pat" >/dev/null 2>&1 \
        && echo "    OK   ${d#$TREE/}/$pat" || echo "    FAIL ${d#$TREE/}/$pat"
    done
  fi
else
  echo "  ⚠️ 未安装 git-lfs：arkui 的 idlizer-*.tgz 会是 131 字节指针，构建必挂在 npm install"
fi

echo "[ensure] 校验"
ls "$PRODUCT" >/dev/null 2>&1 && echo "  ✓ 产品配置就位"
ls "$TREE/productdefine/common/inherit/rich.json" >/dev/null 2>&1 && echo "  ✓ rich.json 就位"

cat <<'TIP'

下一步（产品名是 config.json 里的 product_name，不是目录名！）：
  bash ohos/sa/intree/install_into_tree.sh <TREE> <本仓库 ohos/sa>
  export HOME=${HOME:-/root}                 # FIX-13：hb 的 resolve_ccache 依赖 $HOME，缺失会崩
  cd <TREE> && ./build.sh --product-name x86_64_virt --ccache --ninja-args=-j6
  # ★ 并发必须用 --ninja-args=-j6，不能写 --jobs 6（FIX-14）：
  #   OH 7.0 的 hb 里 resolve_jobs() 是空实现（源码注释 # PlaceHolder），
  #   --jobs 不会传给 ninja，ninja 就会按 CPU 核数并发（本机 16），
  #   12 GB 内存瞬间被 18 个 clang++ 打爆 → 换页 → 慢到 4 目标/分钟。
  #   验证：ps -ef | grep '[n]inja -w dupbuild'  应看到 "images -j6"
  # 不要加 --no-prebuilt-sdk=true：webview（web:*）要链接 prebuilts/ohos-sdk 下的
  # libbundle_ndk.z.so，跳过 SDK 预编译会在 ninja 阶段报 "missing and no known rule"。
TIP
