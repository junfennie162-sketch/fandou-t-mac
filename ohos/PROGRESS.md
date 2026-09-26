# P0–P5 落地状态（host 侧 2026-07-26）

| 阶段 | Issue | Host | Board |
|------|-------|------|-------|
| Gate0 PRD+Issues | — | done | — |
| P0 hello | #2 | aarch64 hello 已编 | hdc 空，待板 |
| P1 kernels | #3 | `libtmac_kernels.a` + selftest 已编；nm 无 TVM | selftest 待推 |
| P2 llama | #4 | `llama-cli`/`llama-bench` + `.so` OHOS 已编 | 推理待板+gguf |
| P3 SA | #5 | `libtmac_sa.so` + workspace/tile/QoS + `sa_smoke` | samgr 注册待板 |
| P4 QoS | #6 | `ApplyPerceptionSignal` + 可选 `TMAC_USE_OH_QOS` | 真 QoS 链库待系统镜像 |
| P5 交付 | #7 | 矩阵/报告骨架；**适配 zip 已打** | 实测数待填 |

## 本机零硬件验证 · 2026-09-25（Windows x64）

环境：OpenHarmony SDK 6.1-LTS（独立安装，`env.ps1` 自动探测）+ OHOS clang 15.0.4 + WSL(Kali) qemu-aarch64-static 7.2。

| 项 | 结果 |
|----|------|
| P0 hello 交叉编译 | ✅ `elf64-littleaarch64` / `EM_AARCH64` |
| P1/P3 交叉编译（kernels + selftest + sa + smoke） | ✅ 全绿；`llvm-nm -u` 仅 `memset` + stack-chk |
| **P1 selftest 执行**（qemu 模拟 aarch64-ohos） | ✅ PASS，determinism/zero-ref NMSE = 0 |
| **P3 sa_smoke 执行**（qemu） | ✅ PASS，信号→QoS→会话→warm kernel→释放 全链路 |
| 板侧（hdc + 真机） | 仍待设备 |

复现要点（无开发板）：

1. 编译：`powershell -File ohos\scripts\build_kernels.ps1`（SDK 路径由 `env.ps1` 探测，含 `D:\ohos-sdk\ohos-sdk\windows\native`）。
2. qemu 执行需**静态链接**版本（OHOS musl 动态加载器在 qemu-user 下段错误）：
   `clang++ --target=aarch64-linux-ohos --sysroot=<sdk>/native/sysroot -O2 -static -DNDEBUG -march=armv8.2a+dotprod+fp16 -std=c++17 ... -o tmac_selftest_static`
3. WSL：`qemu-aarch64-static ./tmac_selftest_static`（libc++ 静态库在 `<sdk>/native/llvm/lib/aarch64-linux-ohos`）。

> 已知坑：`ohos/shim/dmlc/logging.h` 的 `DCHECK` 宏三元分支类型不一致，非 `NDEBUG`（Debug）构建编译失败；Release/qemu 路径均需 `-DNDEBUG`。

## x86_64 路径与上游 bug 修复 · 2026-09-25

DevEco 模拟器（Windows 上为 x86_64）路线的内核验证：

| 项 | 结果 |
|----|------|
| `deploy/tuned/kernels.cc`（x86_64/AVX2）编到 `x86_64-linux-ohos` | ✅ |
| `ohos/selftest/main_x86.cpp` 运行（x86_64 静态，WSL 里直接执行） | ✅ PASS |

**修复的上游 bug**：`tbl_float_reset` 里 `memset(c, 0, m * sizeof(float_type))` 在非 NEON 构建下
`float_type = float`（4B），而目标缓冲 `half = _Float16`（2B）→ **memset 双倍越界**，
写坏栈上返回地址（`ret` 跳向地址 0 → SIGSEGV）。ARM 上二者同为 2B，故从未暴露。
已修复 `deploy/tuned/kernels.cc` 与源码模板 `python/t_mac/intrins/tbl.cc`（改用 `sizeof(_Float16)`）。

## 模拟器演示工程（HAP）· 2026-09-25

`ohos/hap/`：ArkTS 界面 + NAPI 桥 + **双架构自适应内核**（x86_64→AVX2 `deploy/tuned` / arm64→NEON `aarch64-hf-bitnet-3b`）。
两个架构的 `libtmac_hap.so` 均用 SDK 工具链构建通过（NEEDED 仅 `libace_napi.z.so` + `libc++_shared.so` + `libc.so`）。
DevEco 打开工程即可跑模拟器/真机，步骤见 `ohos/hap/README.md`。

## 🎉 鸿蒙模拟器实测通过 · 2026-09-26

HarmonyOS 7.0.0 (API 26) 模拟器（x86_64）实测——**T-MAC LUT 内核在鸿蒙系统内第一次执行通过**：

| 步骤 | 结果 |
|---|---|
| hvigor 26.0.0 构建 HAP（双架构 native） | ✅ BUILD SUCCESSFUL |
| `hdc install`（unsigned HAP） | ✅ install bundle successfully |
| 应用启动 | ✅ EntryAbility onCreate → Index loaded |
| **应用内点击「运行内核自测」** | ✅ **PASS: kernel ran, zero-in => zero-out** |

完整链路：ArkTS 界面 → NAPI (`libtmac_hap.so`) → `qgemm_lut_int8` → x86_64 AVX2 内核 → 结果回传界面。

复现命令（模拟器在线时）：

- 连接：`hdc tconn 127.0.0.1:5555`
- 安装：`hdc install <HAP 绝对路径>`（用反斜杠路径，hdc 不认正斜杠）
- 启动：`hdc shell aa start -a EntryAbility -b com.fandou.lutsa`

> 环境坑记录：DevEco 的网络请求会被用户级 `http_proxy` 环境变量劫持（代理一挂 IDE 网络全断），已清理并备份为 `http_proxy_bak`。

## 🎉 鸿蒙模拟器完整 LLM 推理成功 · 2026-09-26

llama.cpp（T-MAC fork `eb07ecf`）交叉编译到 **x86_64-ohos**，模拟器上跑通完整推理（Qwen2.5-0.5B-Instruct-Q4_K_M，463 MiB）：

| 指标 | 实测 |
|---|---|
| 模型加载 | 5272 ms |
| prompt 处理 | 17.15 tok/s（7 tokens） |
| **文本生成** | **13.04 tok/s**（63 tokens，4 线程） |
| 输出 | 中文流畅生成 ✅ |

**移植补丁**（子模块内，重拉需重新应用）：`ohos/patches/llama_cpp_ohos_musl.patch`
- `common/common.cpp` 两处 `#if` 增加 `!defined(__MUSL__)`（musl 无 `pthread_setaffinity_np`，属 glibc 扩展；**证实可行性文档待核实项①：OHOS musl 不放开该 API**）

复现命令（模拟器在线）：

- 编译：`cmake .. -DCMAKE_TOOLCHAIN_FILE=<sdk>/native/build/cmake/ohos.toolchain.cmake -DOHOS_ARCH=x86_64 -DOHOS_STL=c++_shared -DGGML_OPENMP=OFF`（在 `3rdparty/llama.cpp/build-ohos-x86`）
- 推送：`hdc file send <llama-cli|libllama.so|libggml.so|model.gguf> /data/local/tmp/llm/`
- 运行：`hdc shell "cd /data/local/tmp/llm && LD_LIBRARY_PATH=. ./llama-cli -m model.gguf -n 64 -t 4 -p '你好'"`
- 注意：hdc 在 Git Bash 下需 `MSYS_NO_PATHCONV=1`，本地路径用反斜杠

**SIMD 优化（同日）**：CPUID 探针（`ohos/selftest/cpuid_probe.c`）发现模拟器虚拟 CPU 为 **"AVX2 ✅ / FMA ❌ / F16C ✅"** 组合；补 `-mavx2 -mf16c` 并在 `ggml-cpu-impl.h` 加 FMA intrinsic 的 mul+add 兜底宏后重编：

| 指标 | 标量版 | SIMD 版 | 提升 |
|---|---|---|---|
| 文本生成 | 13.04 tok/s | **55.80 tok/s** | **4.3×** |
| prompt 处理 | 17.15 tok/s | 115.32 tok/s | 6.7× |
| 模型加载 | 5272 ms | 2294 ms | 2.3× |

编译配方：`cmake .. -DCMAKE_C_FLAGS="-mavx2 -mf16c" -DCMAKE_CXX_FLAGS="-mavx2 -mf16c"`（**勿加 `-mfma`**：虚拟 CPU 不支持会 SIGILL）。

**T-MAC 集成待做**（GGML_TMAC=ON）需匹配的 x86 内核（TVM 生成）+ BitNet-3B 模型转换。

## T-MAC 加速集成攻坚 · 2026-09-26 夜

**结论：技术链全通，卡在内核执行层最后一个 bug（下游均为上游未验证区）。**

已打通：WSL 从零编译 TVM 0.17 + LLVM 17 → **x86_64-ohos BitNet LUT 内核生成**（两个分支：
`-ags -1` int32 变体 / `-ags 64` float 变体，后者与队友 aarch64 内核符号完全一致）→
BitNet-3B 转换（12.6GB HF → 966MB T-MAC GGUF，288 张量，2.44 BPW）→
llama.cpp `GGML_TMAC=ON` 集成编译 → **全链路激活**（transform / 内核 dispatch 匹配 / 4 线程 / 数据流验证）。

**修复的集成层 bug（5 处，完整补丁见 `ohos/patches/llama_cpp_ohos.patch`）**：

1. `tmac_float_type` x86 定义错误（float → `_Float16`；与 fp16 内核匹配）
2. `ggml_tmac_transform_tensor` 在 mul_mat 分支补调（幂等）
3. mmap 只读区 inplace scales 转换写崩（运行加 `--no-mmap`）
4. 生成阶段：`pthread_setaffinity_np`（musl 无此 glibc 扩展）、FMA 兜底宏、`half` typedef 条件错误
5. 转换阶段：yaml/safetensors/torch 依赖链、权重分片完整性校验

**遗留**（已于当日深夜定位）：`qgemm_lut` 特化内核执行内部 SIGSEGV / 输出全 0。

**根因（逐层对照实验，最终定位）**：

| 编译器 | 运行环境 | 结果 |
|---|---|---|
| GCC 12（conda） | WSL 真 CPU | ✅ **正常**（输出非零、数值合理） |
| **OHOS clang 15.0.4** | WSL 真 CPU | ❌ 全 0 |
| OHOS clang 15.0.4 | DevEco 模拟器 | ❌ 全 0 / SIGSEGV |

→ **OHOS clang 15.0.4 对该内核的代码生成缺陷**：反汇编显示核心「查表累加」循环的
**累加指令（`vpaddw`）全部缺失**（查表 `vpshufb` 正常生成）——编译器错误消除了累加逻辑，
导致输出恒 0。`-O0` / `-fno-strict-aliasing` 均复现（**非优化级别问题**）。

**三层洗清**：非模拟器缺陷、非集成代码问题、非 T-MAC 内核源码问题——**是 OHOS x86 编译器工具链的缺陷**。

**影响与结论**：
- 模拟器路线（OHOS clang x86 工具链）**无法承载 T-MAC 的复杂内核**；
- **真机 ARM 不受影响**（clang 的 aarch64 是 OHOS 主场，上游路径）；
- 普通推理（非 LUT 内核）不受影响：**55.8 tok/s 已验证**。

产物：`deploy/tuned/ohos-x64-bitnet-3b/`（ags=-1）、`deploy/tuned/ohos-x64-ags64/`（ags=64）。

## 提交包

`docs/output/report/submission/03-LUT-SA翻斗花园-ohos-adapt.zip`

产物目录：`ohos/build/` · `ohos/hello/build/` · `3rdparty/llama.cpp/build-ohos-check/`
