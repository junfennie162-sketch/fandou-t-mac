# LUT-SA · HAP 演示工程（T-MAC on HarmonyOS）

把 T-MAC 的低比特 LLM 推理装进一个正经的鸿蒙应用：**ArkTS 界面 → NAPI → 静态 llama.cpp + LUT 内核**，
真机/模拟器一键运行（DevEco 点 Run 即可）。

> SystemAbility 版本还在 `ohos/sa/`（需要开发板）；这里是"应用形态"。

## 当前状态（2026-09-27）

| 项目 | 状态 |
|---|---|
| HAP 构建（双 ABI，命令行 hvigor） | ✅ `BUILD SUCCESSFUL`，产物 `entry/build/default/outputs/default/entry-default-unsigned.hap`（6.6 MB） |
| `libtmac_hap.so`（x86_64 2.2 MB / arm64 1.8 MB） | ✅ 内含静态 llama.cpp + T-MAC 内核（库内嵌 kcfg） |
| 内核自测 / 基准（`selfTest` / `bench`） | ✅ 代码就绪（沿用已验证的 m128-k3200 形状） |
| LLM 推理（`loadModelAsync` / `generateAsync`） | ✅ 代码就绪；待设备上跑（模拟器原先那份 `model.gguf` 可直接用） |

## 你需要做什么（装 DevEco → 跑模拟器）

1. **安装 DevEco Studio**（Windows 版，developer.huawei.com 下载），安装时让它装好默认 SDK（5.0 / API 12）。
2. **登录华为账号**（免费注册）。模拟器需要账号授权。
3. **打开工程**：`File → Open` 选本目录（`ohos/hap`），等右上角 Sync 完成。
4. **配置签名**：`File → Project Structure → Signing Configs → 勾 Automatically generate signature`（登录后自动完成）。
5. **创建模拟器**：`Device Manager → Local Emulator → New Emulator → Phone，镜像选 x86_64`，下载并启动。
6. **推模型**（966 MB，不进 HAP）：
   ```bash
   hdc file send bitnet-3b-tmac-ags64.gguf /data/local/tmp/llm-tmac/model.gguf
   ```
   （模拟器上一般已有这份；没有就用上面命令推。真机请用 `bitnet-3b-tmac-arm64.gguf`）
7. **运行**：目标选模拟器 → 点 **▶ Run 'entry'**。
8. **推模型**（见下一节，966 MB **必须走沙箱覆盖法**，`/data/local/tmp` 读不了）。
9. **预期结果**（2026-09-27 实测通过）：
   - 「内核自测」→ `✅ 内核自测通过 · PASS: LUT kernel ran, zero-in => zero-out`
   - 「内核基准」→ 200 次内核调用总耗时 / 平均耗时
   - 「加载模型」→ `✅ 模型已加载`，`load = 4956 ms`（含 288 张量内核变换）
   - 「生成」→ `the city of Paris. The city is located in France. Paris was a Roman`
     + `eval ≈10.5 tok/s`（同一设备三次独立测量 10.38 / 10.51 / 10.52，误差 <1.5%；截图：`ohos/screenshots/emulator/hap-demo-screenshot.png`）

## 模型怎么进 App（两种方式）

### 方式 1（产品级，任何设备都能用 ✅ 推荐）：App 内「📂 选择模型文件」

点「📂 选择模型文件」→ 系统文件选择器（可按 `.gguf` 过滤）→ 选中后 App **自己把模型拷进沙箱**并自动加载。
不需要 hdc/shell 权限，不受设备沙箱策略影响——真机同样可用。
（实测：选择器可正常拉起并带隐私提示；取消分支正确。模拟器用户存储里没有文件所以选不了实际文件。）

### 方式 2（开发捷径，仅当设备允许 shell 覆盖 App 文件）：hdc 覆盖沙箱占位

**HarmonyOS 7 实测的限制**：App 读不了 `/data/local/tmp`（SELinux 拦截）；shell 也不能在 App 沙箱**新建**文件。
可行通道：App 启动时 `chmod(沙箱,0777)` 并创建一个 **0666 的 `model.gguf` 占位**，shell 就能**覆盖**它：

```bash
# App 内看到的路径：/data/storage/el2/base/haps/entry/files/…
# shell 侧真实路径： /data/app/el2/100/base/com.fandou.lutsa/haps/entry/files/…
hdc file send bitnet-3b-tmac-ags64.gguf \
  /data/app/el2/100/base/com.fandou.lutsa/haps/entry/files/model.gguf
```
（App 每次启动会把已存在的 `model.gguf` 重新 chmod 到 0666，保证这条通道长期可用。）

> 备选：App 通过 HTTP 从主机下载模型到沙箱（模拟器网关 `10.0.2.2` 实测可达），需加 `ohos.permission.INTERNET`。

## 命令行构建（可选，只产未签名 HAP）

```powershell
$env:DEVECO_SDK_HOME = "D:\DevEco Studio\sdk"
& "D:\DevEco Studio\tools\node\node.exe" "D:\DevEco Studio\tools\hvigor\bin\hvigorw.js" `
    assembleHap --mode module -p product=default --no-daemon
```
安装到设备需签名：IDE 的 Run 会做自动签名；命令行安装要自行用 `hap-sign-tool.jar`。

## 工程结构

```
hap/
├── AppScope/                     # 应用级配置（包名 com.fandou.lutsa）
├── prebuilt/
│   ├── x86_64/{libllama.a,libggml.a}     # AVX2 内核（staging-x64；模拟器无 FMA，勿加 -mfma）
│   └── arm64-v8a/{libllama.a,libggml.a}  # NEON fp16 内核（staging-arm64，armv8.2a+fp16）
└── entry/
    └── src/main/
        ├── ets/
        │   ├── entryability/EntryAbility.ets
        │   └── pages/Index.ets                 # 界面：模型路径/prompt/加载/生成/自测/基准
        ├── cpp/
        │   ├── CMakeLists.txt                  # 按 ABI 链接 prebuilt 静态库（T-MAC 内核在 libggml.a 内）
        │   ├── napi_init.cpp                   # NAPI 桥（同步 + 异步两套入口）
        │   └── types/libtmac_hap/index.d.ts    # ArkTS 类型声明
        └── resources/
```

## 接口

| NAPI | 说明 | 返回 |
|---|---|---|
| `nativeVersion()` | 架构 / tfloat 宽度 / 链接信息 | 字符串 |
| `selfTest()` | 零输入冒烟：跑一次内核，校验零入零出 | 含 `PASS`/`FAIL` |
| `bench(steps)` | 内核重复调用计时（1–100000 次） | 形状 / 总耗时 / 平均 |
| `loadModel(path, filesDir, threads?, nCtx?, useMmap?)` | 同步加载（会阻塞） | 状态字符串 |
| `loadModelAsync(...)` | **异步（Promise）**，UI 用这个 | `Promise<string>` |
| `generate(prompt, nPredict?, temp?, topK?)` | 同步生成 | 文本 + tok/s |
| `generateAsync(...)` | **异步（Promise）**，UI 用这个 | `Promise<string>` |
| `release()` | 释放模型/上下文 | 状态字符串 |

```typescript
import tmac from 'libtmac_hap.so';
const ok = await tmac.loadModelAsync('/data/local/tmp/llm-tmac/model.gguf', ctx.filesDir, 4, 512, false);
const out = await tmac.generateAsync('The capital of France is', 16, 0.8, 40);
```

## 设计要点

- **kcfg 自包含**：内核所需的 `kcfg.ini` 以字符串编进 `libtmac_hap.so`，首次加载时写入应用沙箱并
  `setenv("TMAC_KCFG_FILE", …)`（wrapper 优先读环境变量）→ 不依赖主机构建路径。
- **异步**：加载/生成耗时较长，`*Async` 在 NAPI 工作线程执行，避免卡死 UI（系统对无响应应用有看门狗）。
- **采样器自带**：本 fork 的采样器在 `common/`（未链接进 HAP），NAPI 内实现 top-k + temperature
  （`std::mt19937(42)`，确定性）。
- **模型必须与 ABI 匹配**：

| ABI | 模型 | 说明 |
|---|---|---|
| x86_64（模拟器） | `bitnet-3b-tmac-ags64.gguf` | kcfg 与 `staging-x64` 一致 |
| arm64-v8a（真机） | `bitnet-3b-tmac-arm64.gguf` | kcfg 与 `staging-arm64` 一致（m6400 形状 bm 不同，需按 arm64 重转） |

## 故障排查

| 现象 | 处理 |
|---|---|
| 安装失败 `signature verification failed` | 用 DevEco Run（自动签名），或自行签名后 `hdc install` |
| `FAIL: llama_load_model_from_file` | 路径不对/模型与 ABI 不匹配；先 `hdc shell ls -l <path>` |
| 加载成功但进程被杀（OOM） | UI 打开 `mmap` 开关（权重走文件映射，匿名内存大降） |
| 生成很慢 | 模拟器 x86_64（无 FMA）+ 虚拟化本就慢；真机 arm64+NEON 才是目标环境 |
| `libtmac_hap.so` 加载失败 | 确认设备 ABI（模拟器=x86_64，真机=arm64-v8a）在 `abiFilters` 里 |
| Sync 报 SDK 版本不匹配 | 改 `build-profile.json5` 的 `compatibleSdkVersion` |
| native 编译找不到 `deploy/tuned/...` | 本工程需放在完整仓库里（`ohos/hap` 不能单独拿出仓库） |

## 注意

- 模拟器（x86_64/AVX2，**无 FMA**）与真机（arm64/NEON fp16）是**两套内核代码路径**，报告分开表述。
- 模拟器性能数字只能演示流程；真机数据用 `ohos/scripts/deploy_arm64_device.ps1` 或本 App 在真机跑。

## 端侧落地的「系统服务化」形态：AppServiceExtensionAbility（跨进程调用）

零售版 HarmonyOS **不允许第三方注册 SystemAbility**（实测六条锁：非 root / SELinux Enforcing /
系统分区只读 / profile·init 目录不可访问 / `hdc target mount` 要求 debug 镜像 / 两套 SDK 均无 samgr 头文件），
所以手机上能落地的"系统服务化"形态是 **AppServiceExtensionAbility**：引擎跑在扩展里，
别的组件/应用用 `connectServiceExtensionAbility` 连上来，经 **RPC** 调用 —— 相当于把
「引擎在自己进程里持有模型，UI 进程只发请求」这条系统级设计搬到可上架的应用模型里。

### 新增/改动

| 文件 | 作用 |
|---|---|
| `entry/src/main/ets/engineext/LutEngineExtension.ets` | **服务端**：`AppServiceExtensionAbility` + `rpc.RemoteObject`，把 native 接口包成 RPC 方法 |
| `entry/src/main/ets/engineext/EngineClient.ets` | **客户端**：`connectServiceExtensionAbility` + `MessageSequence` 发请求（别的应用照抄这段即可接入） |
| `entry/src/main/module.json5` | 声明 `extensionAbilities`（`type: "service"`、`exported: true`、`extensionProcessMode: "instance"`） |
| `entry/src/main/ets/pages/Index.ets` | 「模型」页新增 **跨进程调用** 卡片：连接 / 自检 / 断开 |

### RPC 协议

```
请求： writeString(method) + writeString(argsJson)
应答： writeString(JSON.stringify({ok, data?, err?}))
方法： ping / nativeVersion / selfTest / bench / loadModel / generate / release / prepareSandbox
      （loadModel、generate 走 native 的 *Async，onRemoteMessageRequest 返回 Promise，不阻塞 IPC 线程）
```

### 真机验证步骤

1. 用 DevEco 打开本目录 → **Run 'entry'**（IDE 会自动签名；命令行只产未签名 HAP，见上节）。
2. 进「模型」页 → 找到 **🔌 跨进程调用（ServiceExtensionAbility）** 卡片。
3. 点 **连接** → 再点 **自检** → 期望显示
   `✅ 跨进程自检通过（引擎在独立进程）`，正文含版本串、内核自测（PASS）与基准耗时。
4. 点 **断开** → 引擎进程可被系统回收（等于把引擎内存还给系统）。
5. 想看进程隔离：`hdc shell hidumper -ls | grep lut` 或 `ps -ef | grep lutsa`，对照 UI 进程 pid。

### 已验证到哪一步

- ✅ 命令行 `assembleHap`：`CompileArkTS` / `PackageHap` 均 Finished，产出
  `entry/build/default/outputs/default/entry-default-unsigned.hap`（6.85 MB）
- ⚠️ `SignHap` 在本机失败（`Init keystore failed`，本地 keystore 的 JDK 版本问题）——
  这是**环境**问题，用 IDE 自动签名即可；不影响代码本身
- ⏳ 真机点击跑通待你在设备上点一下（代码路径与 SDK 类型已全部按本机 SDK 校验通过）

## 实测结果：HarmonyOS 模拟器（7.0.0 / API 26 / phone）

设备：`DevEco Studio 自带模拟器`（`hdc list targets` → `127.0.0.1:5555`）

| 环节 | 结果 |
|---|---|
| 构建 | ✅ `CompileArkTS` / `PackageHap` Finished |
| 签名 | ✅ SDK 自带 **OH 测试链**在命令行签名（`verify-app: Verify success`），**HarmonyOS 模拟器认这份签名**，不必用 IDE 自动签名 |
| 安装 | ✅ `hdc install -r lutsa-signed.hap` → `install bundle successfully`（图标出现在桌面） |
| 启动 | ✅ `aa start -a EntryAbility -b com.fandou.lutsa` → `start ability successfully` |
| 原生引擎 | ✅ `nativeVersion: LUT-SA native | x86_64 (AVX2, no FMA) | tfloat=4 B | kcfg embedded` |
| **LUT 内核自测** | ✅ `KERNEL-SELFTEST: kernel m=128 k=3200 n=1 b=2 (x86_64 AVX2) | PASS: LUT kernel ran, zero-in => zero-out` |
| **LUT 内核基准** | ✅ `KERNEL-BENCH: m=128 k=3200 n=1 b=2 | steps=200 | total=5.35 ms | avg=0.0267 ms` |

> 自测/基准在应用启动时**自动执行**并写 hilog（tag `LUTSA_KERNEL`），无需点 UI；抓取：
> ```bash
> hdc shell hilog -x | grep -aE 'LUTSA|KERNEL-'
> ```

### ⚠️ 关键发现：AppServiceExtensionAbility 是特权扩展，第三方应用不能用

最初把引擎包成 `AppServiceExtensionAbility`（想做成"独立进程 + 可被别的组件连"的系统服务化形态），
在模拟器上装包被拒：

```
error: failed to install bundle. code:9568344 error: install parse profile prop check error.
BMS 日志真因: bundle_install_checker.cpp:ProcessBundleInfoByPrivilegeCapability:1676
              not allow use privilege extension
```

**HarmonyOS 把 `AppServiceExtensionAbility` 归为 privilege extension，普通第三方应用不允许声明**（这与"零售系统不让第三方注册 SystemAbility"是同一类平台边界）。
所以 `module.json5` 里的 `extensionAbilities` **已注释掉**（原文保留在文件里，附原因说明），端侧引擎就跑在应用进程内。

**可用的替代形态**（第三方应用能做、且有"系统协作"味道的）：

| 方案 | 说明 |
|---|---|
| `childProcessManager` | 应用自己拉起**独立子进程**跑引擎（真进程隔离，模型与会话内存不占 UI 进程），但不能对外暴露成跨应用服务 |
| `taskpool` / Worker | 同进程多线程，最轻；适合把推理放到后台线程 |
| `backgroundTaskManager` | 长时任务保活（本项目已在用：切后台释放/回前台预热） |

**结论**：第三方 App 能做到"引擎独立进程 + 生命周期受控"，但**做不到"对外提供系统级跨进程服务"**——那是系统应用/SA 的领域。这也是本项目"系统级形态"只有两条路的原因：真机侧走应用内进程隔离，系统侧走 OpenHarmony 标准系统里的 SA（见 `../sa/QEMU-DEPLOY.md`）。

### 复现步骤（命令行，无需点 IDE）

```bash
# 1) 构建（SignHap 会因本机 keystore/JDK 失败，属预期，只要未签名包）
cd ohos/hap && node "$DEVECO/tools/hvigor/bin/hvigorw.js" assembleHap --mode module -p product=default --no-daemon

# 2) 用 OH 测试链签名（脚本在仓库 ohsign/ 下）
bash <repo>/ohsign/build_and_sign.sh

# 3) 装 + 起 + 取证
hdc uninstall com.fandou.lutsa        # 若装过旧版（appId 不同会报 sign info inconsistent）
hdc install -r lutsa-signed.hap
hdc shell aa start -a EntryAbility -b com.fandou.lutsa
hdc shell hilog -x | grep -aE 'LUTSA|KERNEL-'
```

> **版本号格式坑**：`build-profile.json5` 里 `compatibleSdkVersion`/`targetSdkVersion`：
> API 10–25 用 `"5.0.0(12)"` 这种带括号的写法，**API ≥ 26 必须写纯版本号 `"26.0.0"`**（写错会报
> `00306042 Specification Limit Violation` 或 `00308018 api version parameter is illegal`）。
