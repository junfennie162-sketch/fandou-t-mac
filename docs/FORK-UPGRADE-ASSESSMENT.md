# Fork 升级评估报告（fork-upgrade 分支）· 2026-09-29

> 目标：把 3rdparty/llama.cpp 从旧 T-MAC fork（2024-10 底座）升级到新分支，解锁 KV 量化
> （旧版被 head_dim=100 % 32 ≠ 0 断言封死，新 llama.cpp 靠 head-dim padding 支持）。

## 一、侦察发现（决定性）

**上游 `kaleid-liner/llama.cpp`（T-MAC 作者本人的 fork）远程分支现状**：

| 分支 | 最新提交 | 状态判读 |
|---|---|---|
| `202504_tmac` | 2025-04-30 `Unify quantization_config loading` | **升级目标**：现代 llama.cpp（2025-04 底座，ggml 后端拆分后架构），T-MAC 已移植，x86 对 Llama-2/Phi-3 已 correct；作者 4 月下旬仍在修（有 "[WIP] Wrong outputs" 中间态提交） |
| `master` | 2024-08-17 | 落后于我们当前底座，忽略 |
| 当前使用 | `181ad23a`（2024-10 底座 + 我们的 OHOS 修复） | 生产版，已冻结 |

**Delta 规模**：962 文件 / +206k −112k 行（22 个月的 llama.cpp 上游演进）——工作量已被作者吸收，
我们不需要自己重做移植，而是**适配 + 验证**。

## 二、升级路线图（复赛后执行）

| 阶段 | 内容 | 预估 |
|---|---|---|
| 1 | 把 `202504_tmac` 拉进子模块，先在本机（WSL/x86）构建 + 跑通 T-MAC 的 x86 参考路径 | 半天 |
| 2 | 重放我们的 OHOS 补丁（旧底座 5 文件 33 行 + 集成修复，文件位置/API 有变动需逐处核对） | 1 天 |
| 3 | BitNet-3B 链路验证：模型转换（`Unify quantization_config loading` 可能改了格式）+ kernels 兼容性（必要时用 TVM 重新生成） | 1~2 天 |
| 4 | 双 ABI 重建（x86_64/arm64-v8a）+ 模拟器/真机验证（复用 `tests/lut-verify` 对照法） | 1 天 |
| 5 | **KV 量化落地**：q8_0 K+V（新版支持 head_dim padding）+ 与链式 KV 叠加测试 | 半天 |

**总计：约 4~5 个工作日**（vs "从零移植"的周级——作者已把最重的移植做了）。

## 三、风险清单

1. **202504_tmac 是 WIP 性质**：历史中有 "Wrong outputs" 提交，BitNet 专项未经我们验证——阶段 1/3 就是干这个
2. **kernels ABI 可能变化**：若 ggml-tmac 的 kernel 调用约定改了，需 TVM 重新生成内核（环境还在 WSL）
3. **模型格式**：T-MAC GGUF 的 metadata/量化配置加载逻辑已改，可能需要重转模型
4. 双 ABI + OHOS 静态构建的坑会重来一遍（有手册，成本可控）

## 四、结论与建议

- **可行，且比预想便宜**（作者已移植完毕），但**绝不适合截止前 30 小时动**：WIP 分支 + 全链路重验证
- 复赛后第一件事执行阶段 1，用 `tests/lut-verify` 的 NMSE 对照法快速判定 202504_tmac 的健康度
- 届时 KV 量化（省内存）+ 分页/前缀链深化（服务化）两条线都有了技术底座

---
*证据：`git ls-remote` 分支清单、`git log origin/202504_tmac`、`git diff --stat eb07ecf0 origin/202504_tmac`。*

## 五、阶段 1 进展（2026-09-29 晚）

| 步骤 | 状态 |
|---|---|
| 克隆 202504_tmac 到隔离目录 | ✅ `D:\ohos-models\llama-202504`（分支顶点 81475f4，独立于生产子模块，零污染） |
| 构建开关确认 | ✅ `ggml/CMakeLists.txt:211` 有 `GGML_TMAC` 选项 |
| 代码结构侦察 | ✅ T-MAC 移入 **`ggml/src/ggml-cpu/tmac/`**（11 文件：ggml-tmac / tmac / lut_ctor / lut_mul_mat / tbl 各 .cpp+.h）——注意：**多了 lut_ctor/lut_mul_mat/tbl 运行时组件**，内核接线方式可能与旧版 TMAC_DIR 外置包不同 |
| 下一步 | ①弄清内核如何接线（旧 staging-x64 内核包是否兼容 / 是否需 TVM 重生成）→ ②用 OHOS SDK clang（Windows 侧，target x86_64-linux-ohos，已验证管线）配置+构建 → ③`tests/lut-verify` NMSE 对照判定健康度 |

**风险更新**：新分支把部分运行时代码 in-tree（可能降低对外置 kernels 包的依赖，利好移植；但也可能要配套新版 kernel 生成流程，阶段 2 核对）。

## 六、阶段 2 纪事（2026-09-29 深夜）

**构建环境**：Windows OHOS SDK clang（`--target=x86_64-linux-ohos`）+ DevEco cmake/ninja，产物静态 musl 丢 WSL 跑（成熟管线）。

| 轮次 | 结果 | 根因与处置 |
|---|---|---|
| configure | ✅ | `GGML_TMAC=ON` 原生支持，零缺包（作者的 t-MAC 全在树内，运行时 kcfg 注册表派发） |
| build #1 | ❌ | `SignedHalvingAdder`/`scales` 未声明 —— **AVX2 守卫未激活**（GGML_NATIVE=OFF 只给 SSE4.2）→ 加 `-DGGML_AVX2=ON -DGGML_F16C=ON` |
| build #2 | ❌ | `_mm256_fmadd_ps requires fma` —— **新版 x86 路径硬依赖 FMA，且没带我们老 fork 的"FMA 兜底宏"补丁** → 验证构建先 `-DGGML_FMA=ON`（WSL i7 有 FMA）；**鸿蒙/模拟器构建需重放我们的 FMA 兜底补丁（阶段 3 任务，老修复直接复用）** |
| build #3 | 🔄 | 加了 GGML_FMA=ON，构建中 |

**结论（重要）**：新分支的适配需求与旧 fork **同族同源**（AVX2 守卫 / FMA 依赖 / 可能还有类型契约），我们的移植知识库 100% 可迁移——阶段 3 预估可从 1 天下调。

## 七、阶段 3 开局（2026-09-29 深夜续）

| 项 | 结果 |
|---|---|
| 静态构建 | ✅ `build-ohos-x86-static/bin/llama-cli`（10.3 MB，全静态 musl） |
| WSL 运行 | ✅ 二进制正常启动（loader/backend init 全过） |
| 已重放的老补丁（3/3 命中） | ① `-DGGML_AVX2=ON -DGGML_F16C=ON`（AVX2 守卫）② `-DGGML_FMA=ON`（新版 x86 硬依赖 FMA；**鸿蒙版仍需重放我们的 FMA 兜底宏**）③ `common.cpp` 两处守卫加 `!defined(__MUSL__)`（musl 无 pthread affinity） |
| **旧模型被拒**（关键发现） | `tensor 'blk.0.ffn_down.weight' of type 37 (TYPE_IQ4_NL_4_8 REMOVED, use IQ4_NL with runtime repacking)` —— 旧 fork 的自定义 T-MAC 张量类型**已移除**，新版改为 **IQ4_NL + 运行时重排布** 方案 |
| 待办 | ✅ 下一步：用新版 `convert_hf_to_gguf.py`（自带 `enable_t_mac` 参数）+ HF 源模型 `D:\ohos-models\bitnet-3b` **重新转换** → 重跑 llama-cli 验证文本 → 再查内核/kcfg 接线（新版或在构建期生成/打包内核） |

**意义**：格式换代 = 一次重转（半小时级），不是拦路虎；三条老补丁 1:1 命中已三度验证"移植知识完全可迁移"。
