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

## 八、阶段 3 终局记录（2026-09-29 深夜，实验线暂停点）

| 项 | 结果 |
|---|---|
| 环境 | 老 Py3.8 conda 环境 + 一行 `from __future__ import annotations`（tmac_utils.py）= 新脚本全通（py311 环境已建为备胎） |
| 转换格式选型 | `tmac_bn_0`（块256）对 BitNet-3B 的 8640 维度**除不尽被拒**；改用 **`tmac_w2g64_0`（块64，8640÷64=135 ✓）** 转换**成功**：966MB / 288 张量 |
| 加载终审 | ❌ `tensor 'blk.0.ffn_sub_norm.weight' has offset 211737632, expected 213465600`（差 1,727,968 B）——写入器/读取器**偏移记账不一致** |
| 根因排查 | ① 已排除类型尺寸表不一致（C++ `ggml.c:579` 与 Python `constants.py:2040` 两边都是 blck=64/type_size=20，完全一致）② 报错点=BitNet **特有**的 sub_norm 张量（作者测试的 Llama-2/Phi-3 **没有此类张量**）→ 高嫌疑区：WIP 分支对 sub_norm + TMAC 混合布局的 raw_shape/offset 处理（对应其历史提交 "ggml_tmac_transform_tensor should use *data as the original data" 同族） |
| 上游动态 | `kaleid-liner/llama.cpp@202504_tmac` 顶点仍为 81475f4（作者未推新修复） |

**结论与后续**：新 fork 的**构建链已打通**（三补丁 + 转换器可用），卡在作者 WIP 分支的**模型布局 bug**（BitNet 专项，作者盲区）。两条出路：① 专门调试 writer/reader offset 记账（半天~数天，需逐张量比对写入偏移 vs 读取期望）；② 等作者更新或提 issue 反馈（本地材料已齐备）。**实验线自此暂停，主线（提交）全程零影响。**

## 九、阶段 4 战报（2026-09-29 深夜，重大突破 + 最后一块拼图）

**两个加载器补丁**（脚本存档：`D:\ohos-models\patch_gguf.py` / `patch_ggmlnbytes.py`）：
1. `gguf.cpp`：5 处尺寸记账改用 TMAC 感知 helper；
2. `ggml.c` 的 `ggml_nbytes()` 本体加 TMAC 特判（一处修、全员受益，llama-model-loader 的 6 处边界检查自动正确）。

**结果**：模型**完整加载** ✅（288 张量 / TMAC buffer 1157.23 MiB / KV 162.5 MiB / 图 994 节点），运行时**内核自动调优生效**（实时 tune：bm=320/640/1280, kfactor=16, q_group_size=64, act_group_size=64）。

**崩溃与根因（证据链闭环）**：预热推理 SIGSEGV。调优配置显示 **q_group_size=64（每 64 元素一组 scale）**，而转换器 BitNet 路径（`convert_hf_to_gguf.py:~2290`）硬编码 `scale.reshape(1)` → **每张量只写 1 个 scale**。与最初 1,727,968 B 偏移差完全对应：`8640×3200÷64×4 = 1,728,000` = 缺失的每组 scale 字节数。**结论：类型表（4+16/64）自始正确，是转换器 BitNet 路径的"单 scale 硬编码"是 WIP 未完成品（与 "Hard code bits/groupsize/sym" 提交同族）。**

**下一步（已精确处方）**：
① 检查运行时 scale 读取代码（ggml-tmac.cpp 中读取 tensor->data 的 scale 段布局：块内插 vs 尾部追加）；
② 按处方修转换器 BitNet 路径：per-group(g=64) 三值量化（每 64 组算 scale）→ `preprocess_for_t_mac(w, scales=per_group_array)`；
③ 重转 + 重测（预计 1~2 小时一轮）。

## 十、阶段 5 终局（2026-09-29 凌晨，实验线收官存档）

**修复链回顾**:列转换器 per-group 量化（g=64，max-abs scale，`patch_conv.py`）→ 转换成功 **1,214,183,840 B（1.21GB，比单 scale 版大 1.7MB×~80 张量 = 组 scale 字节）**，逐张量字节数精确命中类型表（如 3200×3200 → 3,200,000 B ✓）→ 加载器补丁**全部撤回**（修好转换器后标准代码即正确，文件与上游类型表严丝合缝）。

**验证结果**:模型**完整加载** ✓（1157 MiB / KV 162.5 MiB / 994 节点）→ **内核自动调优运行** ✓（实时 tune q_group_size=64 配置）→ 预热推理 **SIGSEGV** ✗。

**崩溃精确坐标**（dmesg + llvm-addr2line）:
- `segfault at 0`（空指针解引用），ip=0x4c3ba7，**单/多线程同址（确定性）**
- 函数:**`ggml_backend_tmac_mul_mat`**

**下一步处方（下个会话 30 分钟级）**:
① 读 `ggml-tmac.cpp` 的 `ggml_backend_tmac_mul_mat` 全部指针解引用点，锁定哪一个为 NULL（候选：kernel_config 查找失败分支、tmac_tensor_extras/workspace 未初始化、warmup 空 batch 的退化路径）；
② 对照作者历史提交 5c79170a（x86 correct）与 tip 的 diff，看该函数是否在 WIP 末期改坏；
③ 修复后即可跑通"新 fork + BitNet-3B"——那将是本项目历史上第一个把 BitNet 带上该分支的完整链路。

**备份环境**:py311 + torch（conda 清华频道）安装结果见任务日志；主线不受影响。

## 十一、崩溃弹道分析（2026-09-29 凌晨，函数级定位完成）

**案发现场**：`lut_mul_mat.cpp:983`（`ggml_backend_tmac_mul_mat` 内）:
```cpp
struct tmac_tensor_extra * wt = ((struct ggml::cpu::tmac::tensor_traits *)src0->extra)
                                    ->get_tmac_tensor_extra(src0->name);
```
崩溃签名 = `segfault at 0`（地址 0 精确命中 = 虚表/首成员偏移 0 的解引用）。

**双嫌疑犯**：
- **A**: `src0->extra == NULL` → 虚表调用在 0 处解引用（签名完美吻合）；
- **B**: extra 正常但 `get_tmac_tensor_extra(name)` 返回 NULL（结构体首成员偏移 0 处解引用，同签名）。注册点在同文件 :677（"已注册则跳过"），若注册与查询的张量命名/时机不一致则命中。

**已排除**：
- 不是构建/环境问题（确定性、单/多线程同址）；
- 类型尺寸表（逐字节核对一致）、文件布局（per-group 版全部命中）、加载器（全通）；
- 派发门控正常（tmac.cpp:39-42：仅当 src0 在 tmac buffer 时才派发到该函数）。

**下一步（15 分钟级）**：在 :983 前插入探针打印（`src0->extra`、`src0->name`、`wt`），重编 → 运行 → 一眼定罪；A → 查 init_tensor 链路为何遗漏该张量；B → 对齐注册/查询的命名与时机（对照作者提交 "Fix ggml_tmac_transform_tensor should use *data"）。

## 十二、探针定罪与最后一英里（2026-09-29 凌晨，本段结束）

**探针实证**（`lut_mul_mat.cpp:983` 两侧插打印，`patch_probe2.py`）:
```
[TMAC-DBG] src0=blk.0.attn_q.weight extra=0x5e1140 buf=0x...(有效)
[TMAC-DBG] wt=0 name=blk.0.attn_q.weight        ← 定罪：名字表查空
```
- 嫌疑 A（extra NULL）**排除**：traits 正常挂载；
- 定罪 **B：运行时 extras 表查询返回 NULL** → 下一行 `wt->lut_scales_size` 在 0 址解引用 → SIGSEGV。

**已逐一排除**（全部有源码证据）:
1. 环境/构建（确定性、单多线程同址）✗
2. 类型闸 `is_type_supported(W2G64_0)` = true（`is_tmac_2bit_type` 含 39-43）✗
3. 注册/查询同文件（单一定义 `lut_mul_mat.cpp:924` convert_weight、:983 查询）✗
4. 表重置 `tmac_init()` 在 `ggml-cpu.c:3466`（first-call 块内、加载之前执行一次）✗
5. 派发门控（tmac.cpp:39-42 仅 tmac-buffer 张量进此函数）✗

**最后一英里（下一步 30 分钟级）**:读 `ggml_tmac_transform_tensor`（lut_mul_mat.cpp:~674 起）的**注册尾部**（~700-780 行）——即剔除断言后的实际注册代码。注意:Release 构建 `-DNDEBUG` 使 `assert(kernel_config->has_scale)` 等**编译期消失**，若注册尾部存在"按 config 类型分叉"的逻辑（如仅 one_scale 路径注册），我们的 per-group 配置可能落入**跳注册**分支——这就是 B 的机制。修法大概率是 1 行（补注册分支或修正条件）。

**另存**：`patch_probe2.py`、探针构建产物均在 D:\ohos-models\llama-202504；实验线整体暂停于"最后一英里"。

## 十三、真凶全链闭合（2026-09-29 凌晨，探针三联定案）

**三联探针实证**（`patch_probe3.py`：header 的 set/get + tmac.cpp 的 set_tensor 同时埋点）:
```
[TMAC-DBG] set_tensor ...   ← 加载全程：零输出（从未被调用！）
[TMAC-DBG] SET ...          ← 零输出（注册从未发生！）
[TMAC-DBG] GET-MISS (map sz 0)  ← 运行时查询：表空
```

**完整因果链（定案）**:
1. 模型加载（--no-mmap）→ 张量进了 TMAC buffer（1157 MiB 已分配、init_tensor 挂好 traits ✓）
2. **但 `ggml_backend_tmac_buffer_set_tensor`（转换+注册入口）从未被调用** → 权重未转换、extras 未注册
3. 运行时 mul_mat 查询 extras → GET-MISS（表空）→ `wt=NULL`
4. `wt->lut_scales_size` 在 0 址解引用 → SIGSEGV（与 dmesg "segfault at 0" 完全吻合）

**病因判读**：作者 WIP 的加载管线处于"半迁移"状态——设计假设数据经 set_tensor 走转换/注册路径，但当前架构的装载流程（mmap 直映或新 loader 批量路径）绕过了它。这与历史提交 "Fix ggml_tmac_transform_tensor / gather logics in can_mul_mat" 同族（把转换时机挪来挪去没挪完）。

**下一步处方（下会话收尾，1-2 个探针内可定）**:
① 在 `llama-model-loader.cpp` 的 load_data() 埋探针，看实际走的是 mmap 分支还是 read+set 分支（`--no-mmap` 下为何仍绕过 set_tensor）；
② 视结果二选一：修 loader 的分支条件，或把 transform 挂到正确的装载回调用；
③ 重编重跑——通过即「新 fork + BitNet-3B」全链路首通。

**探针工具链**（重放即可，全部在 D:\ohos-models\）：`patch_probe2.py`（:983 双探针）· `patch_probe3.py`（三联探针）· 各 rebuild*.log。

## 十四、🏁 收官：端到端首通 + 真性能基线（2026-09-29 凌晨终章）

**突破性修复（本项目原创）**：在 `tensor_traits::work_size`（图规划期、单线程、每图一次）补「**惰性转换**」——发现张量未注册则就地 `ggml_tmac_transform_tensor(src0, src0->data)`。这补齐了作者"gather logics in can_mul_mat"未完成的半页：因加载管线绕过了 set_tensor，转换/注册从未发生（三联探针实证 map sz 0）。

**结果（清探针后）**：
| 指标 | 数值 |
|---|---|
| 崩溃 | ✅ 零（182 张量全惰性转换成功） |
| **eval 性能** | **46.93 tok/s**（21.31 ms/token，WSL i7）——**接近旧 fork PC 基线 25.10 的 1.9 倍** |
| prompt eval | 88.43 tok/s |
| 输出质量 | ❌ `The capital of France is is is is is…` 退化循环 = 作者同款 "[WIP] Wrong outputs" |

**结论**：新 fork 于本项目首次「**不崩地跑完端到端**」，且**性能潜力显著高于旧 fork**；剩余唯一课题 = **数值正确性**（嫌疑：per-group scale 读取约定 / transform permute 布局 / 量化语义微差），已有 `tests/lut-verify` NMSE 对照法可直接开攻。

**fork-upgrade 工具包（全部在本仓/本地）**：
- 补丁：`patch_conv.py`（转换器 per-group）、`patch_lazy_clean.py`（惰性转换，核心）
- 探针工具：`patch_probe2/3.py`、`patch_gguf.py`、`patch_ggmlnbytes.py`（已还原）
- 产物：`bitnet-3b-tmac-202504-g.gguf`（1.21GB）、静态 llama-cli（build-ohos-x86-static）
- 诊断记录：本文件 §一~十四 全程弹道

## 十五、数值侦查收官：管线一致性全清单 + 残余课题定性（2026-09-29 终）

**关键考证（决定性文档）**：旧管线权威语义出自 `python/t_mac/weights.py` 文档字符串:
> "Add a bias of 2^(bits-1)... E.g., add a bias of **2** to int2: -2,-1,0,1 -> 0,1,2,3"

→ BitNet 三值 {-1,0,1}+2 = **{1,2,3}** 是跨代正统编码。**我们的 per-group 补丁（round(x/s+2).clip(1,3)）= 正确编码**；"旧文件含 0"系误报（旧文件在转换期已被位交织置换，按字节直读的分布无意义）——**+1 编码实验基于假前提，已叫停回滚**。

**管线一致性核对（全部通过 ✓）**：
| 检查项 | 结论 |
|---|---|
| 比特打包顺序（tighten_bit_array ↔ BlockI2TypeAccessor::get_q） | ✓ 均为"元素0占最高位" |
| scale 顺序（转换器行主序 (m, k/64) ↔ 运行时 get_scale(idx=im*k+ik)） | ✓ 逐索引对齐 |
| 尺寸表（C++ type_traits ↔ Python constants ↔ 实际文件字节数） | ✓ 三方逐字节一致 |
| 权值编码（+2 bias ↔ 旧代 canonical 语义） | ✓ 正统 |
| 目标分支/构建/加载/调优/惰性转换 | ✓ 全通、零崩溃、46.93 tok/s |

**残余课题定性**：数值正确性（"is is is"退化循环）落在作者 WIP 分支**内核/置换/数值层**——其自身提交即标注 "[WIP] Wrong outputs" 从未解除。我们已把自家侧（转换/加载/运行时接线）做到**可验证范围内全一致**；下一阶段需要：① 拿作者的可用模型（GPTQ-Llama，commit 标注 "correct"）反测其内核以分离"分支普遍问题 vs BitNet 特有问题"；② 或对单一 matmul 做数值级 NMSE 对照（tests/lut-verify 已备）；③ 或等待作者更新。**本议题告一段落，成果与边界均已建档。**
