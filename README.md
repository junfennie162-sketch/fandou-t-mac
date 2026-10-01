# LUT-SA · 端侧低比特 LLM 推理

> **LUT-SA**（应用名「鸿蒙玲珑核」）：比特级查找表（LUT）驱动的低比特 LLM 端侧推理，落地 **HarmonyOS / 安卓 / 桌面 Linux** 三平台。上游开源出处见文末致谢。

> **目标形态**：做成 OpenHarmony / HarmonyOS 的**系统服务**（SystemAbility，SA_ID 6901）——推理能力注册进系统服务框架、开机由 init 拉起、任意应用经 SAMgr 取 proxy 调用，而不是某个 App 的内部功能。
> **当前状态**：应用级形态已交付（HAP，真机实测见下表）；系统级形态已在 OpenHarmony 7.0 源码树中编译通过（9 源文件 → `libtmac_sa.z.so` + 注册表 + init 配置 + SELinux 策略），标准系统镜像构建推进中 → [`ohos/sa/QEMU-DEPLOY.md`](ohos/sa/QEMU-DEPLOY.md)。

---

## 📊 实测数据（同一模型 · 同一内核）

**模型**：BitNet-b1.58-3B 三值权重，**2.44 BPW / 986 MB**
**内核**：LUT-SA 查表内核（x86_64 AVX2 与 arm64 NEON 双架构，自测 PASS；对照官方 NumPy 参考 NMSE 8.4e-05）

### A. 本项目的实现（LUT-SA）

| 载体 | 设备 / 芯片 | 生成速度 | 模型加载 | 数值校验 |
|---|---|---|---|---|
| 鸿蒙真机 | HUAWEI HBN-AL00（Pura 70 Pro）· arm64 | **22.00 tok/s** | **1.69 s** | ISA 安检通过 |
| 安卓真机 | vivo V2323A · 骁龙 8 Gen 2 · Android 16 | 11.55 tok/s（持续 ~10.6） | 1.46 s | **NMSE 8.397e-05** |
| 桌面 | i7-14650HX · WSL · 4 线程 | 25.10 tok/s | 8.2 s | NMSE 8.765e-05 |
| 鸿蒙模拟器 | x86_64 · 4 vCPU | 11.7 tok/s | 1.4~3.7 s | — |

> 鸿蒙真机（交付版 HAP）补充：二轮首字延迟 **TTFT 1.32 ms**（链式 KV 复用 6/6、前缀处理 0.00 ms）· 峰值内存 **1395.1 MB** · 内核基准 200 次 **1.56 ms（7.8 µs/次）** · 安装/启动/渲染三闸门 + 离线验签通过。
> 模拟器数字受宿主机负载影响很大：同配置实测区间约 **10.2 ~ 25.0 tok/s**；跨平台结论一律以真机为准。

### B. 对照组（基线）：同机 llama.cpp 的"反量化"路径

**比的是什么**：拿**同一个源模型**，在**同一台机器**上（桌面 i7-14650HX、4 线程、同提示词与生成长度），分别用两条计算路径跑，比生成吞吐：

| 路径 | 做法 |
|---|---|
| **llama.cpp（基线）** | 先把低比特权重**反量化回高精度**，再做浮点乘加 —— 生态里最常用的做法 |
| **LUT-SA（本项目）** | 权重比特**直接作为查表下标**，一次查表 + 一次整数累加，不反量化 |

| 对照项 | 量化方式 | 权重位宽 | 模型体积 | 生成速度 | 加载 |
|---|---|---|---|---|---|
| llama.cpp | Q4_0（社区最常用的量化档） | ~4.63 bit | 1.79 GiB | 15.94 tok/s | 18.7 s |
| llama.cpp | f16（不量化，精度上限参照） | 16 bit | 6.19 GiB | 4.41 tok/s | 67.9 s |
| **LUT-SA** | 三值 + LUT 查表 | **2.44 bit** | **986 MB** | **25.10 tok/s** | **8.2 s** |

**同机结论**：2.44 bit 的 LUT-SA 比 4.63 bit 的 Q4_0 快 **1.57×**、比 16 bit 的 f16 快 **5.7×**；体积分别是它的 **1/1.9** 与 **1/6.6**。

> **口径说明**：Q4_0 的精度高于本实现（4.63 bit vs 2.44 bit），所以这是一次"体积 / 算力 vs 精度"的权衡对比，不是同精度对比；同精度对照见 [`tests/lut-verify/`](tests/lut-verify/)。
> 此前表格里写的"桌面对手盘"**就是指本节 B 组的这两行**——即"对照组 / 基线"的意思，现已按上面的定义写明。

### C. 三个结论

1. **手机上的 LUT-SA（22.00 tok/s）超过桌面 CPU 上的 llama.cpp Q4_0（15.94 tok/s）**——端侧低比特让手机达到桌面级吞吐
2. 同一个静态二进制 + 同一份模型，**横跨 鸿蒙 / 安卓 / qemu 三环境零改动运行**
3. 冷数据下 LUT-SA 与 Q4_0/Q8_0 的**每字节带宽持平** → 加速全部来自 **2.44 BPW 的位宽优势**

> 鸿蒙模拟器上的同设备 A/B：LUT-SA **11.7 tok/s** vs llama.cpp Q4_0 **5.5 tok/s** → **2.1×**（同模型、同设备、同一会话）。

## 🆕 最新进展（2026-09 落地）

| 能力 | 结论（同设备、单变量） | 入口 |
|---|---|---|
| **链式 KV 记忆调度** | 跨轮匹配最长公共前缀 + 分歧裁剪：二轮 TTFT **240 ms → 1.32 ms**、前缀处理归零；追加式提问部分复用 **6 of 13** | [`ohos/FULL-REPORT.md`](ohos/FULL-REPORT.md) |
| **L2 前后台感知调度** | 切后台归还 **1.30 GB**（Pss 1370→71 MB）、回前台自动预热重载 1.37 s；60 s 滞回护住 KV 链；**对照组（关调度）0 释放** | 同上 |
| **采样器工程化** | 补齐重复惩罚（1.15 / last_n 64）+ 真 top-p 0.95，修掉长文复读退化（同提示 A/B 有据） | 同上 |
| **构建优化（纯 CLI）** | 剔除 DevEco 调试态注入的 ASan 插桩：同机同模型 **10.5 → 25.01 tok/s（2.4×）**；`hvigorw` 一键可复现 | [`ohos/hap/README.md`](ohos/hap/README.md) |
| **OpenHarmony 标准系统载体线** | SA 组件编入 OH 7.0 源码树（`vendor/ohemu/lutsa`）；构建链踩坑 **15 条**全部定位并修复（含 hb `--jobs` 空实现、LFS 指针、SDK 后处理、上游缺陷绕过） | [`ohos/sa/QEMU-DEPLOY.md`](ohos/sa/QEMU-DEPLOY.md) |

---

## 🚀 按平台开始（单仓 · 专区制）

| 平台 | 入口 | 内容 |
|---|---|---|
| 🐾 **鸿蒙** | [`ohos/FULL-REPORT.md`](ohos/FULL-REPORT.md) | 全程总结（五阶段/13+8 问题清单/数据）· HAP 应用：[`ohos/hap/README.md`](ohos/hap/README.md) |
| 🤖 **安卓** | [`docs/ANDROID-FIX.md`](docs/ANDROID-FIX.md) | 「能跑不输出」五大坑排坑 + NDK/adb 全配方 + 真机验证记录 |
| 💻 **桌面 x86** | [`docs/PC-BUILD.md`](docs/PC-BUILD.md) | 三方基准（4.41/15.94/25.10）+ TVM 内核生成/模型转换/构建全配方 |
| 🔬 **跨平台验证** | [`tests/lut-verify/`](tests/lut-verify/) | 内核对照参考法工具 + 全部 NMSE 实测记录 |

---

## 📁 仓库结构

```
fandou-t-mac/
├── docs/
│   ├── ANDROID-FIX.md        # 安卓专区：排坑指南 + 真机记录
│   ├── PC-BUILD.md           # 桌面专区：构建与三方基准
│   ├── knowledge.md / adr/   # 技术调研与决策记录
│   └── output/report/        # 历史报告归档
├── ohos/                     # 🐾 鸿蒙专区
│   ├── FULL-REPORT.md        #   ★ 全程技术总结（先读这个）
│   ├── PROGRESS.md           #   逐日过程记录
│   ├── BENCH-*.md            #   三份基准报告（PC 三方/模拟器 A/B/arm64 验证）
│   ├── hap/                  #   HAP 应用工程（ArkTS + NAPI + 双 ABI）
│   ├── screenshots/          #   真机/模拟器截图证据
│   ├── patches/              #   llama.cpp 集成补丁
│   ├── staging-{x64,arm64}/  #   双架构内核 CMake 包
│   └── selftest/ scripts/    #   探针与一键部署脚本
├── tests/lut-verify/         # 🔬 跨平台内核验证工具 + 实测记录
├── deploy/tuned/             # TVM 生成内核（x64-ags64 / aarch64-hf 等）
├── python/                   # LUT-SA 量化与代码生成（上游）
├── 3rdparty/llama.cpp/       # llama.cpp 子模块（打补丁构建）
└── t-man/                    # NPU 扩展（上游）
```

---

## 一句话创意

把比特级查找表（LUT）驱动的低比特 LLM 推理范式封装为 OpenHarmony / HarmonyOS 用户态系统能力，用轻量行为感知做动态资源调度，把首字延迟和能耗在端侧压下去。

## 系统架构

四层垂直单向依赖（文字版）：

```
L1 应用层        ArkUI 应用（对话 / 控制台 / 模型三页）· 以及任意调用方
      │  NAPI（应用级形态）        │  SAMgr proxy / IPC（系统级形态）
      ▼                            ▼
L2 感知与调度    前后台状态、内存压力信号 → QoS 档位、60 s 滞回保护记忆链
      ▼
L3 系统服务层    LUT SystemAbility（SA_ID 6901）：CreateSession / LoadModel /
                 PrepareWorkspace / WarmKernel / InferTokenBatch / ReleaseSession
      ▼
L4 计算核心      LUT 查表内核（m128-k3200，2.44 BPW，x86_64 AVX2 + arm64 NEON）
```

两条调用路径：

- **应用级形态（当前已交付）**：L1 应用 ──NAPI──▶ L4 计算核心（推理在应用进程内）
- **系统级形态（目标）**：L1 任意应用 ──SAMgr 取 proxy（IPC）──▶ L3 SA ──▶ L4 计算核心（能力在系统侧，调用者在应用侧）

对照与验证：与 llama.cpp 反量化基线对照（见上节 B 组）。

## 调用时序

一次生成的完整链路（文字版）：

```
调用方（应用 / 测试程序）
  │ ① CreateSession            建立会话与工作区
  │ ② LoadModel                模型进内存（真机 1.69 s，含张量内核变换）
  │ ③ PrepareWorkspace         LUT 表与瓦片池就位
  │ ④ WarmKernel               预热查表内核
  │ ⑤ InferTokenBatch × N      逐批推理；每个 token 经 threadsafe function 回吐调用方
  │ ⑥ ReleaseSession           释放会话
  ▼
TTFT 计量区间：CreateSession 完成 → 首 token 返回
             （首轮真机 248.80 ms；二轮命中前缀复用后 1.32 ms）
```

链路节点：`调用方 → SAMgr → LUT SA → Native 库 → LUT 内核`（应用级形态下第 2、3 跳由 NAPI 桥替代）。

## 上游开源项目基线与致谢（参考）

> 本项目已在鸿蒙模拟器 / qemu-arm64 / 零售真机 / 安卓真机完成自有实测（见上表）；以下为上游 T-MAC 官方数据，仅作跨平台参照。单位 tokens / sec。

| 模型 | 设备 | 线程 | llama.cpp | T-MAC | 约倍速 |
|---|---|---|---|---|---|
| BitNet-3B | M2-Ultra | 1 | 6.49 | 22.08 | ~3.4× |
| BitNet-3B | M2-Ultra | 4 | 22.09 | 54.46 | ~2.5× |
| BitNet-3B | Raspberry Pi 5 | 1 | 1.37 | 8.03 | ~5.9× |
| BitNet-3B | Raspberry Pi 5 | 2 | 2.71 | 11.09 | ~4.1× |
| Llama-2-7B (W2) | M2-Ultra | 1 | 3.82 | 16.68 | ~4.4× |
| Llama-2-7B (W2) | AGX Orin | 1 | 0.79 | 4.36 | ~5.5× |

## 移植路线（已全部落地 ✅）

| 步骤 | 内容 | 状态 |
|---|---|---|
| 1 | 抽取 LUT Kernel 为 Native 静态/动态库 | ✅ 双 ABI 静态库（x86_64 / arm64-v8a），`ohos/staging-{x64,arm64}/` |
| 2 | DevEco Native 模块，打通最小推理调用 | ✅ HAP App（ArkTS + NAPI），模拟器与真机均跑通 |
| 3 | 封装 SystemAbility，暴露 Load / Infer / Metrics | ✅ `libtmac_sa.so` + 六入口（Create/Load/Prepare/Warm/Infer/Release）+ 感知 QoS 策略；组件/注册描述/SELinux 策略齐备，并按 OH 真实头文件与 IDL **编译零错误**。系统载体线（编入 OH 标准系统镜像 + QEMU 点亮）进行中：零售真机受 SELinux 与签名限制（FULL-REPORT §6.1），正确载体为 OpenHarmony 标准系统镜像，构建配方与 15 条踩坑见 [`ohos/sa/QEMU-DEPLOY.md`](ohos/sa/QEMU-DEPLOY.md) |
| 4 | 与 llama.cpp 量化路径对照 | ✅ 同模型 A/B 2.1× + 桌面三方对比 + 内核级微基准（200 次 7.1/7.8 µs） |
| 5 | 固化本平台 tokens/s 等数据 | ✅ 交付版真机 **22.00 tok/s / 加载 1.69 s / 二轮 TTFT 1.32 ms / 峰值 1395.1 MB** 实测归档（截图 + 控制台原始报告，22 项证据） |

移植全程（13 处上游/集成 bug 修复、五阶段验证方法学、诚实性声明）：**[`ohos/FULL-REPORT.md`](ohos/FULL-REPORT.md)**

## 项目介绍

低比特大模型在边缘设备落地时，主流路径仍以反量化后高精度乘加实现，反量化访存与转换开销显著抵消低比特收益；多任务并发场景下，首字延迟（TTFT）与能耗指标进一步劣化。这类工作的价值在于：在现有操作系统架构下提升系统 AI 任务运行效率，而纯应用层 Demo 难以承载「系统级创新」这一命题。

LUT-SA 以开源 T-MAC（EuroSys 2025）比特级查找表范式为计算引擎，将低比特混合精度矩阵乘转换为查表与加法运算，消除反量化乘加路径。计算内核经 NDK 封装后，以用户态 SystemAbility 注册到 OpenHarmony / HarmonyOS 系统服务框架，对外统一暴露模型加载、推理与指标回传三类能力。调度侧联动系统通知、前台状态与负载信号，动态调整 AI 任务优先级与预取策略，形成「感知 → 调度 → 加速」闭环。系统路径全程运行于用户态，不依赖未公开的内核接口。

工程实现分三层：计算层适配 T-MAC LUT Kernel 至 ARM 架构，处理访存模式与 LUT 表布局；服务层按 SAMgr 注册 LUT SystemAbility，对外暴露统一加速接口，应用按需获取 proxy；策略层将行为信号映射为优先级、预取与节流策略，可在演示场景中直观呈现。

公开评测数据表明，相对 llama.cpp 反量化基线，T-MAC 在多种边缘 CPU 上吞吐具备稳定优势。**本队已完成自主复现与实测**：鸿蒙模拟器同模型 A/B（T-MAC 2.1× 于 Q4_0）、qemu-arm64 功能验证（内核 NMSE 8.4e-05）、华为零售真机端到端 **22.00 tok/s**、安卓真机 11.55 tok/s——详见 [`ohos/FULL-REPORT.md`](ohos/FULL-REPORT.md)。

在此基础上进一步把系统侧能力做实：**链式 KV 记忆调度**（重复提问首字延迟 240 ms → 1.32 ms）、**前后台感知调度**（后台归还 1.30 GB，对照组零释放）、**采样质量工程化**（修掉长文复读），并完成 OpenHarmony 标准系统载体线的构建攻关（15 条踩坑修复，见 [`ohos/sa/QEMU-DEPLOY.md`](ohos/sa/QEMU-DEPLOY.md)）。

项目意义在于将 LUT 计算范式产品化为系统级服务，降低端侧大模型部署门槛，为隐私本地推理、低功耗生成等场景提供基础支撑。

## 测试报告

**目的**：验证比特级查找表（LUT）相对反量化基线在边缘 CPU 上的吞吐优势，并说明将该能力封装为 OpenHarmony / HarmonyOS 用户态系统服务后的复现与移植路径，支撑「系统级 AI 任务效率提升」这一结论。

**指标**：

| 指标 | 含义 |
|---|---|
| tokens / s | 生成吞吐 |
| NMSE | 内核输出 vs NumPy 参考答案 |
| NUM_THREADS | 参与计算的 CPU 线程数 |
| TTFT | 首字延迟（真机实测） |

**复现入口**（全部实测可复现）：

| 数据集 | 复现入口 |
|---|---|
| 桌面三方对比（f16 / Q4_0 / T-MAC） | [`ohos/BENCH-native-3way.md`](ohos/BENCH-native-3way.md) · [`docs/PC-BUILD.md`](docs/PC-BUILD.md) |
| 鸿蒙模拟器同模型 A/B + 内核级微基准 | [`ohos/BENCH-tmac-vs-simd.md`](ohos/BENCH-tmac-vs-simd.md) |
| aarch64 三层验证（内核/构建/端到端） | [`ohos/ARM64-VALIDATION.md`](ohos/ARM64-VALIDATION.md) |
| 安卓真机（内核 + 端到端） | [`docs/ANDROID-FIX.md`](docs/ANDROID-FIX.md) · [`tests/lut-verify/android-*-run.txt`](tests/lut-verify/) |
| HAP 应用（DevEco 打开即跑） | [`ohos/hap/README.md`](ohos/hap/README.md) |
| 全程总结（真机数据 + 13 处修复清单） | [`ohos/FULL-REPORT.md`](ohos/FULL-REPORT.md) |

**风险与边界**：

| 风险 | 说明 |
|---|---|
| 上游数据与自有实测已分列 | 早期曾引用上游公开数据；现自有实测（模拟器/qemu/真机×2）已归档并标注测量方法 |
| ISA / 内存布局差异 | 已处理：arm64 按 fp16 契约 + kcfg 重转模型 + 强制 armv8.2a+fp16（防真机 SIGILL） |
| 零售真机环境限制 | SELinux 禁 CLI 执行与 hilog，已以 HAP 应用 + 屏幕控制台方案落地（FULL-REPORT §6） |
| 权限边界 | SystemAbility 能力级别以可申请 / 可演示为准 |

> 历史报告全文：[`docs/output/report/phase1-test-report.md`](docs/output/report/phase1-test-report.md)

## 项目与维护

| 项 | 内容 |
|---|---|
| 项目 | **LUT-SA**（应用名「鸿蒙玲珑核」） |
| 维护者 | 聂君奋 |
| 贡献者 | 范腾达、郑李惠杰 |

## 深入材料索引

| 维度 | 入口 |
|---|---|
| 全程技术总结（先读） | [`ohos/FULL-REPORT.md`](ohos/FULL-REPORT.md) |
| 逐日过程记录 | [`ohos/PROGRESS.md`](ohos/PROGRESS.md) |
| 实测截图证据（真机/模拟器） | [`ohos/screenshots/`](ohos/screenshots/) |
| 项目一句话 / 边界 | [`AGENTS.md`](AGENTS.md) · [`CONTEXT.md`](CONTEXT.md) |
| 技术调研与方案 | [`docs/knowledge.md`](docs/knowledge.md) |
| 决策记录 | [`docs/adr/0001-lut-systemability-path.md`](docs/adr/0001-lut-systemability-path.md) |
| 调研与历史材料 | [`docs/output/report/`](docs/output/report/) |
| 任务跟踪 | GitHub Issues（`Aafff623/fandou-t-mac`） |

## 上游 T-MAC 原文

<details>
<summary>点击展开上游 T-MAC 项目正文</summary>

> 上游演示（BitNet on M2-Ultra：T-MAC vs llama.cpp）见 [microsoft/T-MAC](https://github.com/microsoft/T-MAC) 仓库首页。

> 上游曲线（Surface Laptop 7：BitNet / Phi-3.5 随核数变化的 tokens/s）见其仓库 `assets/` 目录。

### What T-MAC does

A lookup-table based kernel library for mixed-precision matrix multiplication on CPU. It replaces dequantize-then-multiply with table lookup and shift-add, so 1/2/4-bit weight × int8/fp16/fp32 activation runs natively without re-casting. Supports BitNet 1.58-bit, BitDistiller/EfficientQAT W2A16, and GPTQ/gguf W4A16 on Apple Silicon, x86, and ARM (Windows / Linux / macOS).

On Surface Laptop 7, 3B BitNet hits 20 tokens/s on a single core and 48 tokens/s on four cores (4~5x llama.cpp). Raspberry Pi 5 still manages 11 tokens/s.

> 上游曲线（T-MAC vs llama.cpp：线程数 vs tokens/s）见其仓库 `assets/` 目录。

[Full profile data](docs/profiling_data.md) covers Surface Laptop 7, M2-Ultra, Jetson AGX Orin, Raspberry Pi 5, Surface Book 3.

### Heterogeneous baselines

Same Llama-2-7B (W2) on Jetson AGX Orin (NUM_THREADS=12 for CPU):

| Framework | Throughput (tok/s) | Power (W) | Energy (J/tok) |
|-----------|:-------------------|:----------|:---------------|
| llama.cpp (CPU) | 7.08 | 15.0 | 2.12 |
| llama.cpp (GPU) | 20.03 | 30.8 | 1.54 |
| T-MAC (CPU) | 15.62 | 10.4 | 0.66 |

Snapdragon X Elite (Llama-2-7B-W4, 1024-in / 1024-out): T-MAC CPU 12.6 tok/s @ 2 cores, 18.7 @ 4 cores, 22 @ max frequency. NPE NPU baseline: 10.4 tok/s.

### Install & run

Requirements: Python 3.8 (TVM), virtualenv, cmake ≥ 3.22.

```bash
git clone --recursive https://github.com/microsoft/T-MAC.git
cd T-MAC
python -m venv .venv && source .venv/bin/activate    # PowerShell: .venv\Scripts\Activate.ps1
pip install -e . -v
source build/t-mac-envs.sh                            # downloads clang+llvm, builds TVM
```

Full platform-specific steps (OSX / Ubuntu / Windows x64 / Windows ARM64 / Android cross-compile) live in their original upstream README sections. Verify with:

```bash
python -c "import t_mac; print(t_mac.__version__); from tvm.contrib.clang import find_clang; print(find_clang())"
```

End-to-end inference via llama.cpp integration:

```bash
pip install 3rdparty/llama.cpp/gguf-py
huggingface-cli download 1bitLLM/bitnet_b1_58-3B --local-dir ${model_dir}
python tools/run_pipeline.py -o ${model_dir} -q int_n
```

GPTQ models use `-m gptq-auto` or a preset name; benchmark with `3rdparty/llama.cpp/build/bin/llama-bench` or `tools/bench_e2e.py`.

NPU extension: see [t-man/README.md](t-man/README.md). Upcoming features: [v1.0.0 plan](https://github.com/microsoft/T-MAC/issues/45).

### Cite

```bibtex
@misc{wei2024tmaccpurenaissancetable,
      title={T-MAC: CPU Renaissance via Table Lookup for Low-Bit LLM Deployment on Edge},
      author={Jianyu Wei and Shijie Cao and Ting Cao and Lingxiao Ma and Lei Wang and Yanyong Zhang and Mao Yang},
      year={2024},
      eprint={2407.00088},
      archivePrefix={arXiv},
      primaryClass={cs.DC},
      url={https://arxiv.org/abs/2407.00088},
}
```

</details>
