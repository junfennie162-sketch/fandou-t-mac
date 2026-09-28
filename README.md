# LUT-SA · 鸿蒙高校创新赛

> 作品技术名 **LUT-SA（应用名「鸿蒙玲珑核」）**，队伍 **翻斗花园（中北大学）**，参赛「鸿蒙高校创新赛 · 方向四 · 操作系统智能创新」——比特级查找表（LUT）驱动的低比特 LLM 推理，落地 **HarmonyOS / 安卓 / 桌面 Linux** 三平台端侧。上游开源出处见文末致谢。

<p align="center">
  <img src="assets/poster/poster-phase1.png" width="85%" alt="LUT-SA 系统海报" />
</p>

---

## 🏆 成绩总览（全平台实测 · 同一 BitNet-b1.58-3B · 2.44 BPW · 966 MB）

| 平台 | 设备/芯片 | 生成速度 | 加载 | 内核数值 |
|---|---|---|---|---|
| **鸿蒙真机** 🏆 | HUAWEI HBN-AL00 · 麒麟 · HarmonyOS 6.1.1 | **18.62 tok/s** | 1.96 s | ✅ isa 安检 |
| 安卓真机 | vivo V2323A · SD 8 Gen 2 · Android 16 | 11.55 tok/s（持续 ~10.6） | 1.46 s | **NMSE 8.397e-05** |
| 桌面 | i7-14650HX · WSL · 4 线程 | 25.10 tok/s | 8.2 s | NMSE 8.765e-05 |
| 桌面对手盘 | 同机 llama.cpp Q4_0（4.63 BPW / 1.79 GiB） | 15.94 tok/s | 18.7 s | — |
| 桌面对手盘 | 同机 llama.cpp f16（16 BPW / 6.19 GiB） | 4.41 tok/s | 67.9 s | — |
| 鸿蒙模拟器 | x86_64 · 同模型 A/B | LUT-SA 11.7 vs Q4_0 5.5 → **2.1×** | 1.4~3.7 s | — |

**三个杀手级结论**：

1. **手机上的 LUT-SA（18.62）超过桌面 CPU 上的 Q4_0（15.94）** —— 端侧低比特让手机达到桌面级吞吐
2. 同一静态二进制 + 同一模型**横跨 鸿蒙 / 安卓 / qemu 三环境零改动运行**（2026-09-28 实证）
3. 冷数据下 LUT-SA 与 Q4_0/Q8_0 每字节带宽持平 → 加速全部来自 **2.44 BPW 位宽优势**（无争议机理解释）

<p align="center">
  <img src="ohos/screenshots/device/device-home-icon.jpg" width="260" alt="鸿蒙真机桌面：LUT-SA 已安装" />
  <img src="ohos/screenshots/device/device-console-02.png" width="260" alt="App 屏幕控制台（真机实测会话）" />
</p>

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
│   ├── knowledge.md / adr/   # 赛题调研与决策记录（初赛材料）
│   └── output/report/        # 初赛报告归档
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

四层垂直单向依赖：L1 应用层 → L2 感知与调度 → L3 系统服务层（用户态 SystemAbility）→ L4 计算核心（LUT-SA 二开 LUT Kernel）。右侧并列「对照与验证」，与 llama.cpp 反量化基线对照。

<p align="center">
  <img src="assets/images/readme/architecture-phase1.png" width="85%" alt="LUT-SA 系统架构图" />
</p>

> 源文件：`assets/images/readme/architecture-phase1.drawio` · 设计说明：`assets/images/readme/architecture-phase1.md`

## 调用时序

五条 lifeline：Demo App → SAMgr → LUT SA → Native Lib → LUT Kernel。一次完整 Generate 走 1 到 5 步：CreateSession → InitBuffers / LoadLUT → Generate → InferTokenBatch → mpGEMM / LUT lookup。右侧 TTFT 标尺覆盖 CreateSession 完成 → 首 token 返回。

<p align="center">
  <img src="assets/images/readme/sequence-phase1.png" width="85%" alt="LUT-SA 调用时序图" />
</p>

> 源文件：`assets/images/readme/sequence-phase1.drawio` · 设计说明：`assets/images/readme/sequence-phase1.md`

## 上游开源项目基线与致谢（参考）

> 本队已在鸿蒙模拟器 / qemu-arm64 / 零售真机 / 安卓真机完成自有实测（见顶表），以下为上游 T-MAC 官方数据，仅作跨平台参照。单位 tokens / sec。

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
| 3 | 封装 SystemAbility，暴露 Load / Infer / Metrics | ✅ `libtmac_sa.so` + sa_smoke（qemu 全链路）；最终交付以 HAP 应用形态——零售真机 SELinux 限制 CLI/SA 通道（见 FULL-REPORT §6.1） |
| 4 | 与 llama.cpp 量化路径对照 | ✅ 同模型 A/B 2.1× + 桌面三方对比 + 内核级微基准 |
| 5 | 固化本平台 tokens/s 等数据 | ✅ 真机 18.62 tok/s 实测归档（截图 + 原始输出） |

移植全程（13 处上游/集成 bug 修复、五阶段验证方法学、诚实性声明）：**[`ohos/FULL-REPORT.md`](ohos/FULL-REPORT.md)**

## 作品介绍

低比特大模型在边缘设备落地时，主流路径仍以反量化后高精度乘加实现，反量化访存与转换开销显著抵消低比特收益；多任务并发场景下，首字延迟（TTFT）与能耗指标进一步劣化。赛题要求在现有操作系统架构下提升系统 AI 任务运行效率，纯应用层 Demo 难以承载「系统级创新」这一命题。

作品 LUT-SA 以开源 T-MAC（EuroSys 2025）比特级查找表范式为计算引擎，将低比特混合精度矩阵乘转换为查表与加法运算，消除反量化乘加路径。计算内核经 NDK 封装后，以用户态 SystemAbility 注册到 OpenHarmony / HarmonyOS 系统服务框架，对外统一暴露模型加载、推理与指标回传三类能力。调度侧联动系统通知、前台状态与负载信号，动态调整 AI 任务优先级与预取策略，形成「感知 → 调度 → 加速」闭环。系统路径全程运行于用户态，不依赖未公开的内核接口。

工程实现分三层：计算层适配 T-MAC LUT Kernel 至 ARM 架构，处理访存模式与 LUT 表布局；服务层按 SAMgr 注册 LUT SystemAbility，对外暴露统一加速接口，应用按需获取 proxy；策略层将行为信号映射为优先级、预取与节流策略，可在演示场景中直观呈现。

公开评测数据表明，相对 llama.cpp 反量化基线，T-MAC 在多种边缘 CPU 上吞吐具备稳定优势。**本队已完成自主复现与实测**：鸿蒙模拟器同模型 A/B（T-MAC 2.1× 于 Q4_0）、qemu-arm64 功能验证（内核 NMSE 8.4e-05）、华为零售真机端到端 18.62 tok/s、安卓真机 11.55 tok/s——详见 [`ohos/FULL-REPORT.md`](ohos/FULL-REPORT.md)。

作品意义在于将 LUT 计算范式产品化为系统级服务，降低端侧大模型部署门槛，为隐私本地推理、低功耗生成等场景提供基础支撑。

## 测试报告

**目的**：验证比特级查找表（LUT）相对反量化基线在边缘 CPU 上的吞吐优势，并说明将该能力封装为 OpenHarmony / HarmonyOS 用户态系统服务后的复现与移植路径，支撑作品「系统级 AI 任务效率提升」主张。

**指标**：

| 指标 | 含义 |
|---|---|
| tokens / s | 生成吞吐 |
| NMSE | 内核输出 vs NumPy 参考答案 |
| NUM_THREADS | 参与计算的 CPU 线程数 |
| TTFT | 首字延迟（复赛补测） |

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
| 上游数据与自有实测已分列 | 初赛曾引用上游公开数据；现自有实测（模拟器/qemu/真机×2）已归档并标注测量方法 |
| ISA / 内存布局差异 | 已处理：arm64 按 fp16 契约 + kcfg 重转模型 + 强制 armv8.2a+fp16（防真机 SIGILL） |
| 零售真机环境限制 | SELinux 禁 CLI 执行与 hilog，已以 HAP 应用 + 屏幕控制台方案落地（FULL-REPORT §6） |
| 权限边界 | SystemAbility 能力级别以可申请 / 可演示为准 |

> 初赛报告全文：[`docs/output/report/phase1-test-report.md`](docs/output/report/phase1-test-report.md)

## 团队

| 项 | 内容 |
|---|---|
| 团队名称 | 翻斗花园 |
| 所属机构 | 中北大学 |
| 参赛赛道 | 模型与算子赛道 |
| 队长 | 聂君奋 |
| 队员 | 范腾达、郑李惠杰 |
| 报名时间 | 2026-07-17 |

## 深入材料索引

| 维度 | 入口 |
|---|---|
| 全程技术总结（先读） | [`ohos/FULL-REPORT.md`](ohos/FULL-REPORT.md) |
| 逐日过程记录 | [`ohos/PROGRESS.md`](ohos/PROGRESS.md) |
| 实测截图证据（真机/模拟器） | [`ohos/screenshots/`](ohos/screenshots/) |
| 项目一句话 / 边界 | [`AGENTS.md`](AGENTS.md) · [`CONTEXT.md`](CONTEXT.md) |
| 赛题与方案 | [`docs/knowledge.md`](docs/knowledge.md) |
| 决策记录 | [`docs/adr/0001-lut-systemability-path.md`](docs/adr/0001-lut-systemability-path.md) |
| 调研与初赛材料 | [`docs/output/report/`](docs/output/report/) |
| 任务跟踪 | GitHub Issues（`Aafff623/fandou-t-mac`） |

## 上游 T-MAC 原文

<details>
<summary>点击展开上游 T-MAC 项目正文</summary>

<h3 align="center">
    <img src="assets/demo.gif">
    <p><a href=https://huggingface.co/1bitLLM/bitnet_b1_58-3B>BitNet</a> on M2-Ultra with T-MAC (LUT-based) vs llama.cpp (dequantization-based)</p>
</h3>

<h3 align="center">
    <img src="assets/e2e_surface7_bitnet_phi.png">
    <p>BitNet and Phi-3.5 tokens/s with # of CPU cores on Surface Laptop 7</p>
</h3>

### What T-MAC does

A lookup-table based kernel library for mixed-precision matrix multiplication on CPU. It replaces dequantize-then-multiply with table lookup and shift-add, so 1/2/4-bit weight × int8/fp16/fp32 activation runs natively without re-casting. Supports BitNet 1.58-bit, BitDistiller/EfficientQAT W2A16, and GPTQ/gguf W4A16 on Apple Silicon, x86, and ARM (Windows / Linux / macOS).

On Surface Laptop 7, 3B BitNet hits 20 tokens/s on a single core and 48 tokens/s on four cores (4~5x llama.cpp). Raspberry Pi 5 still manages 11 tokens/s.

<h3 align="center">
    <img src="assets/e2e_threads.png">
    <p>T-MAC vs llama.cpp, threads vs tokens/s</p>
</h3>

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
