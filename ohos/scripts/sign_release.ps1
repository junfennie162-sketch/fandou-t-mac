# Release-sign the HAP so ANY HarmonyOS device in developer mode can install it.
#
# Why: DevEco's automatic signing produces a DEBUG profile that carries a device-id
# whitelist (our phone + the emulator). A reviewer's device is not on that list and
# `hdc install` fails with 9568423. A RELEASE profile has no device list, so the HAP
# installs anywhere.
#
# One-time prerequisites (AGC console, see docs/RELEASE-SIGNING.md):
#   1. upload lutsa-release.csr -> download the release certificate  -> lutsa-release.cer
#   2. add a release Provision Profile for bundle com.fandou.lutsa    -> lutsa-release.p7b
#   3. drop both files into $MaterialDir (default C:\Users\NJF\.ohos\release-lutsa)
#
# Usage:
#   powershell -File ohos\scripts\sign_release.ps1
#   powershell -File ohos\scripts\sign_release.ps1 -OutName my.hap

param(
  [string]$RepoRoot   = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
  [string]$UnsignedHap = '',
  [string]$MaterialDir = 'C:\Users\NJF\.ohos\release-lutsa',
  [string]$KeyAlias    = 'lutsa-release',
  [string]$AppCert     = '',
  [string]$Profile     = '',
  [string]$Keystore    = '',
  [string]$SignTool    = 'D:\DevEco Studio\sdk\default\openharmony\toolchains\lib\hap-sign-tool.jar',
  [string]$OutDir      = 'C:\Users\NJF\Desktop\t-mac\交付-翻斗花园-20260930\01-HAP包',
  [string]$OutName     = '鸿蒙玲珑核-LUT-SA-v1.2-翻斗花园-发布签名.hap',
  [int]$CompatibleVersion = 12
)

$ErrorActionPreference = 'Stop'
if (-not $UnsignedHap) { $UnsignedHap = Join-Path $RepoRoot 'ohos\hap\entry\build\default\outputs\default\entry-default-unsigned.hap' }
if (-not $AppCert)  { $AppCert  = Join-Path $MaterialDir 'lutsa-release.cer' }
if (-not $Profile)  { $Profile  = Join-Path $MaterialDir 'lutsa-release.p7b' }
if (-not $Keystore) { $Keystore = Join-Path $MaterialDir 'lutsa-release.p12' }
$pwdFile = Join-Path $MaterialDir 'keypwd.txt'

foreach ($f in @($UnsignedHap, $AppCert, $Profile, $Keystore, $pwdFile, $SignTool)) {
  if (-not (Test-Path $f)) { throw "missing: $f" }
}
$pwd = (Get-Content $pwdFile -Raw).Trim()

# Guard: a debug profile whitelists device UDIDs -> a reviewer could not install it.
$profileText = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($Profile))
if ($profileText -match '"type"\s*:\s*"debug"') {
  throw "this is a DEBUG profile (device-id whitelist). Request the RELEASE profile in AGC — see docs/RELEASE-SIGNING.md"
}
if ($profileText -notmatch '"type"\s*:\s*"release"') {
  Write-Warning "profile type is not 'release' — double-check the downloaded .p7b"
}
if ($profileText -notmatch 'com\.fandou\.lutsa') {
  Write-Warning "bundle name com.fandou.lutsa not found in the profile — wrong app?"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$out = Join-Path $OutDir $OutName
if (Test-Path $out) { Remove-Item $out -Force }

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$signArgs = @(
  '-jar', $SignTool, 'sign-app', '-mode', 'localSign',
  '-keyAlias', $KeyAlias, '-keyPwd', $pwd,
  '-appCertFile', $AppCert, '-profileFile', $Profile,
  '-inFile', $UnsignedHap, '-signAlg', 'SHA256withECDSA',
  '-keystoreFile', $Keystore, '-keystorePwd', $pwd,
  '-outFile', $out, '-compatibleVersion', "$CompatibleVersion", '-signCode', '1'
)
& java @signArgs
if ($LASTEXITCODE -ne 0) { throw "sign-app failed (exit $LASTEXITCODE)" }

$verifyArgs = @(
  '-jar', $SignTool, 'verify-app', '-inFile', $out,
  '-outCertChain', (Join-Path $env:TEMP 'lutsa-verify.cer'),
  '-outProfile', (Join-Path $env:TEMP 'lutsa-verify.p7b')
)
& java @verifyArgs
if ($LASTEXITCODE -ne 0) { throw "verify-app failed (exit $LASTEXITCODE)" }
$sw.Stop()

$sha    = (Get-FileHash $out -Algorithm SHA256).Hash
$shaLow = $sha.ToLower()
$sizeMB = [math]::Round((Get-Item $out).Length / 1MB, 2)
Write-Host ""
Write-Host "OK  $out"
Write-Host "    size   : $sizeMB MB"
Write-Host "    sha256 : $shaLow"
Write-Host "    signed + verified in $([math]::Round($sw.Elapsed.TotalSeconds,1)) s"
Write-Host "    proof  : install it on a device that is NOT on the debug whitelist (reviewer's view)"
