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

## 提交包

`docs/output/report/submission/03-LUT-SA翻斗花园-ohos-adapt.zip`

产物目录：`ohos/build/` · `ohos/hello/build/` · `3rdparty/llama.cpp/build-ohos-check/`
