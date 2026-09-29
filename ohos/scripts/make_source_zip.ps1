# Build the competition source package (self-contained).
#
# Why the engine is vendored: 3rdparty/llama.cpp is a git submodule pinned to a
# local-only commit (181ad23a, OHOS port) that is not reachable from any public
# remote, so `git submodule update` cannot restore it for a reviewer. The zip
# therefore carries the full engine source tree, plus the working-tree overlay we
# actually built the HAP with.
#
# Usage:
#   powershell -File ohos\scripts\make_source_zip.ps1
#   powershell -File ohos\scripts\make_source_zip.ps1 -OutDir D:\somewhere -Branch main

param(
  [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
  [string]$OutDir   = 'C:\Users\NJF\Desktop\t-mac\交付-翻斗花园-20260930\02-源代码',
  [string]$Branch   = 'main'
)

$ErrorActionPreference = 'Stop'
$engineRepo = Join-Path $RepoRoot '3rdparty\llama.cpp'

$stage = Join-Path $env:TEMP ("lutsa-src-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$pkg   = Join-Path $stage 'LUT-SA-源码-翻斗花园'
$tree  = Join-Path $pkg 'fandou-t-mac'
New-Item -ItemType Directory -Force -Path $tree | Out-Null

# 1) main repo tree (committed state of $Branch)
$repoZip = Join-Path $stage 'repo.zip'
git -C $RepoRoot archive --format=zip -o $repoZip --prefix=fandou-t-mac/ $Branch
Expand-Archive -Path $repoZip -DestinationPath $pkg -Force

# 2) vendored engine = committed engine tree + working-tree overlay
#    (git archive zip + Expand-Archive; do NOT use tar here — an MSYS tar inherited
#     from a Git-Bash PATH reads "C:\..." as a remote host and silently extracts nothing)
$engine = Join-Path $tree '3rdparty\llama.cpp'
New-Item -ItemType Directory -Force -Path $engine | Out-Null
$engineZip = Join-Path $stage 'llama.zip'
git -C $engineRepo archive --format=zip -o $engineZip HEAD
Expand-Archive -Path $engineZip -DestinationPath $engine -Force
$engineHead = (git -C $engineRepo rev-parse --short HEAD).Trim()
$dirty = @()
$dirty += git -C $engineRepo diff --name-only
$dirty += git -C $engineRepo diff --cached --name-only
foreach ($f in ($dirty | Where-Object { $_ } | Select-Object -Unique)) {
  $src = Join-Path $engineRepo $f
  $dst = Join-Path $engine $f
  New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
  Copy-Item $src $dst -Force
  Write-Host "overlay: $f"
}

# 3) manifest / readme for the reviewer
#    Single-quoted here-string on purpose: inside a DOUBLE-quoted one a backtick escapes
#    the next character, so markdown code spans break the text (`$var stayed literal,
#    `f turned into a form feed). Values are injected through placeholders instead.
$repoHead = (git -C $RepoRoot rev-parse --short HEAD).Trim()
$date     = Get-Date -Format 'yyyy-MM-dd HH:mm'
$readme = @'
# LUT-SA 源码包说明（鸿蒙玲珑核）

**作品**：鸿蒙玲珑核 · LUT-SA 端侧低比特 LLM 推理
**赛事**：2026 中国高校计算机大赛 · AI 创意赛 · 鸿蒙赛道
**队伍**：翻斗花园（中北大学）
**打包**：__DATE__

## 目录结构
- `fandou-t-mac/` … 主仓库（评审入口：README.md、ohos/、docs/、deploy/）
  - `ohos/hap/` … DevEco 工程（ArkTS UI + NAPI + 预编译静态库）
  - `ohos/hap/prebuilt/{arm64-v8a,x86_64}/lib{llama,ggml}.a` … 预编译引擎静态库（LUT 内核已内嵌）
  - `ohos/staging-{arm64,x64}/t-mac/include` … LUT 内核头文件
  - `ohos/sa/` … 系统服务化（SystemAbility）预研
  - `ohos/scripts/` … 构建/打包脚本（含本包生成脚本 make_source_zip.ps1）
  - `docs/SUBMISSION-CHECKLIST.md` … 提交清单与实测记录
  - `docs/RELEASE-SIGNING.md` … 可选：发布签名（AGC）流程
- `fandou-t-mac/3rdparty/llama.cpp/` … **引擎全量源码（已内嵌）**
  - 基线 fork：kaleid-liner/llama.cpp @ master-rebased
  - 含提交 __ENGINE__：ohos 移植修复（构建系统的 OHOS 适配、内核接入）
  - 说明：该目录在原仓库是 git submodule，指向上述**本地提交**（上游不可达）→ 本包内嵌全量源码，**无需联网拉子模块**。

## 提交坐标
- 主仓库：__REPO__（分支 __BRANCH__）
- 引擎：__ENGINE__

## 签名口径
- 组委会通知（2026-09-30）：未上架作品优先 Debug 签名；硬性要求 = 必须提交**已签名**的包。
- 本作品未上架，提交的 HAP 为 Debug 签名（hap-sign-tool verify-app 通过）；发布签名流程为可选项（docs/RELEASE-SIGNING.md）。

## 构建 HAP（命令行复刻，与提交版一致）
``````powershell
$env:DEVECO_SDK_HOME = "D:\DevEco Studio\sdk"   # 指向 DevEco 安装自带 SDK
cd fandou-t-mac\ohos\hap
& "D:\DevEco Studio\tools\node\node.exe" "D:\DevEco Studio\tools\hvigor\bin\hvigorw.js" --mode module -p module=entry@default -p product=default -p requiredDeviceType=phone assembleHap
# 产物：entry/build/default/outputs/default/entry-default-signed.hap（需先在 IDE 配置签名）
``````

注意：**不要**附加 `-p ohos-debug-asan=true`（DevEco 的 Run 会自动加，导致 ASan 插桩、推理约慢 2.4×；提交版 HAP 为纯净构建）。

## 重建原生静态库（可选）
见 `fandou-t-mac/ohos/scripts/*.ps1` 与 `ohos/ARM64-VALIDATION.md`；LUT 内核由 t-mac 代码生成流程产出（详见仓库根 README 与 docs/）。
'@
$readme = $readme.Replace('__DATE__', $date).Replace('__REPO__', $repoHead).Replace('__ENGINE__', $engineHead).Replace('__BRANCH__', $Branch)
Set-Content -Path (Join-Path $pkg '源码包说明.md') -Value $readme -Encoding UTF8

# 4) zip — zip_tree.py writes the UTF-8 name flag for the Chinese paths; Compress-Archive
#    (PowerShell 5.1) does not, which shows up as mojibake in Explorer on GBK systems.
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir 'LUT-SA-源码-翻斗花园.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
$zipPy = Join-Path $PSScriptRoot 'zip_tree.py'
& python $zipPy $pkg $zip
if ($LASTEXITCODE -ne 0) {
  Write-Warning "python zip failed, falling back to Compress-Archive (names may lack UTF-8 flags)"
  Compress-Archive -Path $pkg -DestinationPath $zip -CompressionLevel Optimal
}

$sizeMB = [math]::Round((Get-Item $zip).Length / 1MB, 1)
$sha    = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Host ""
Write-Host "OK  $zip"
Write-Host "    size   : $sizeMB MB"
Write-Host "    sha256 : $sha"
Write-Host "    repo   : $repoHead / engine: $engineHead"
