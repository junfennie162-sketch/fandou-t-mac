# 发布签名（AGC 申请指引）—— 让任何鸿蒙设备都能安装

> **状态：可选流程（2026-09-30 更新）**。组委会通知明确「**未上架的作品优先使用 Debug 签名**」，
> 本作品未上架，因此提交的是 Debug 签名包（已验签）——**本流程不是必须**。
> 仅在以下场景执行：需要让评委在任意设备（非 UDID 白名单）直接安装、或官方后续明确要求发布签名。

> 目标：交付的 HAP 在**评委的设备**上也能 `hdc install` 成功（官方通知第 2 条）。

## 为什么现在是装不上的

- 当前 HAP 是 DevEco **自动签名**（调试证书 + 调试 Profile）。调试 Profile 里带
  `debug-info.device-ids` 设备白名单——目前只有 2 台：本队手机 + 官方模拟器镜像。
- 评委设备不在白名单 → `hdc install` 报 **9568423**（unauthorized device）。
- **发布证书 + 发布 Profile 没有设备白名单**，任何开启开发者模式的鸿蒙设备都能装。

## 一、本机已备好的材料

目录 `C:\Users\NJF\.ohos\release-lutsa\`（**不要外传、不要进仓库**）：

| 文件 | 用途 |
|---|---|
| `lutsa-release.p12` | 发布密钥库（私钥在本地，AGC 不会拿到） |
| `keypwd.txt` | 密钥库/密钥口令（签名脚本自动读取，无需手输） |
| `lutsa-release.csr` | **上传给 AGC** 的证书请求文件 |

## 二、AGC 操作（约 10 分钟，登录开发者账号）

入口：<https://developer.huawei.com/consumer/cn/service/josp/agc/index.html> → 「我的项目」

1. **创建项目**（名字随意，如 `LUT-SA`）→ 在项目内 **添加应用**：
   - 平台：AppGallery Connect；类型：HarmonyOS 应用
   - **应用包名必须逐字等于 `com.fandou.lutsa`**
   - 设备类型：勾选与 HAP 一致（手机 / 平板 / 2in1）
2. **申请发布证书**：「证书、App ID 和 Profile」→「证书」→「新增证书」
   - 类型选 **发布证书**，上传 `lutsa-release.csr` → 下载 `.cer`
3. **申请发布 Profile**：「Profile」→「添加」
   - 类型选 **发布**；关联第 1 步的应用；选第 2 步的证书 → 下载 `.p7b`
4. 把下载到的两个文件按下面名字放回 `C:\Users\NJF\.ohos\release-lutsa\`：

   | 下载物 | 重命名为 |
   |---|---|
   | 发布证书链 `.cer` | `lutsa-release.cer` |
   | 发布 Profile `.p7b` | `lutsa-release.p7b` |

> 更习惯 DevEco 图形界面也行（Project Structure → Signing Configs → 取消自动签名 →
> 手动「新建证书」），但**不要与上面这套密钥混用**，且 IDE 会往 `build-profile.json5`
> 写签名配置（该文件含密钥材料，永远不要提交到公开仓库）。

## 三、一键出发布版 HAP

```powershell
cd C:\Users\NJF\Desktop\t-mac\fandou-t-mac-main
powershell -File ohos\scripts\sign_release.ps1
```

脚本流程：**校验 Profile 是 release 类型**（防手滑下错成调试 Profile）→ 校验 bundle
name → `sign-app`（`-compatibleVersion 12`）→ `verify-app` → 输出到交付目录
`01-HAP包\鸿蒙玲珑核-LUT-SA-v1.2-翻斗花园-发布签名.hap` → 打印 SHA256。

## 四、验收标准（必须做一次）

- 把发布版 HAP 装到**不在调试白名单**的设备上（评委视角）：
  `hdc install -r "…-发布签名.hap"` 必须成功。
- 在白名单设备（本机手机/模拟器）上装成功**不能**证明评委能装——务必找一台
  非白名单设备实测一次；没有第二台设备时，可以请同学 / 另一台鸿蒙手机帮忙验证。

## 五、常见问题

| 现象 | 处理 |
|---|---|
| AGC 提示需实名认证 | 开发者账号完成个人实名认证即可申请发布证书 |
| 装包报 Profile / 签名校验失败 | 确认 AGC 里应用的包名逐字为 `com.fandou.lutsa`，且 Profile 关联的是这台应用的发布证书 |
| 发布版与调试版互相覆盖报 9568332 | 同一设备上先 `bm uninstall -n com.fandou.lutsa` 再装 |
| Profile 有效期 | 发布 Profile 通常远长于调试 Profile（调试仅 14 天），以 AGC 显示为准；比赛窗口内无忧 |
