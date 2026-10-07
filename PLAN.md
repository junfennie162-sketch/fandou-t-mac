# LUT-SA 总计划与进度板（单一进度源）

> 这个文件是**唯一进度源**：人看的计划、定时任务每次"读我 → 做一件事 → 写我"的状态都在这里。
> 详细的踩坑账本在 [`ohos/sa/QEMU-DEPLOY.md`](ohos/sa/QEMU-DEPLOY.md)（FIX-1…58），证据在 [`ohos/sa/evidence/`](ohos/sa/evidence/)。
> 给自动化任务看的运行规则在文末「六、定时任务运行规则」——**每轮先读那一节**。

---

## 零、目标与"什么叫做成了"

**总目标**：把 LUT-SA 做成**鸿蒙（OpenHarmony 标准系统）里的系统级能力** —— 一个 SA（`SA_ID=6901`），
任何应用/系统组件经 samgr 就能调用它做**端侧本地模型推理**（模型与屏幕内容都不出设备）。

**最终畅想（不排期，只作方向）**：系统里能对本地模型下命令 —— *"小度，打开豆包、打开微信"* ——
它自己解析意图并**执行系统级动作**。这要求 SA 先"能推理、能被调、能执行动作"三件事都稳。

**"行"的判据（每一档都要可复现的硬证据，不接受口头结论）**：

| 档 | 判据 |
|---|---|
| 能注册 | `GetSystemAbility(6901)` 成功（samgr 里有它） |
| 能被调 | 另一个进程跨 IPC 调 6 个方法，失败项 0 |
| 能推理 | `LoadModel` ErrCode=0 + `Generate` 出真 token + 同 prompt 可复现 + 换 prompt 不同 |
| 能执行动作 | 自然语言意图 → 结构化动作 → 真的启动了目标 Ability（有回执） |
| 稳 | 冷启动 N 轮全绿 + 异常输入不崩 + 一份不合适的模型不能把 SA 打死 |

---

## 一、架构现状（高内聚、低耦合的边界）

```
   调用方（应用 / 系统组件 / 以后的小艺式入口）
        │  samgr LoadSystemAbility(6901) + ILutSa IPC
        ▼
┌──────────────────────────────────────────────┐
│ system/lib64/libtmac_sa.z.so   ← SA 进程宿主库 │
│  ┌────────────────────────────────────────┐  │
│  │ 业务层（本仓库 ohos/sa/*.cpp，GN 编）     │  │
│  │  会话 / 工作区 / QoS / 瓦片计算 / 自检    │  │
│  │  只依赖一个 C 接口：engine_shim.h         │  │
│  └───────────────┬────────────────────────┘  │
│                  │ extern "C"（异常不外泄）    │
│  ┌───────────────▼────────────────────────┐  │
│  │ 引擎静态库 libllama_engine.a（GN 外编）  │  │
│  │  engine_shim.cc + gguf_admission.cc     │  │
│  │  llama.cpp + ggml + t-mac LUT 内核       │  │
│  └────────────────────────────────────────┘  │
└──────────────────────────────────────────────┘
  证据通道：/data/lut_sa/rt_*.txt（SA 的 stderr）+ 串口 ttyS0（guest 内一次性取证服务）
```

**耦合纪律**（这就是"地基"）：
1. **业务层不出现 llama 的 C++ 类型、不出现 try/catch**（GN 编，`-fno-exceptions`）；换引擎只改 `engine_shim`。
2. **引擎不在 GN 里编**：OH 工具链强制 `-fno-exceptions` 而 llama.cpp 用 `throw`；且两套工具链 libc++ ABI
   不同（DevEco `std::__n1` vs OH `std::__h`）→ 引擎由 `intree/build_engine.sh` 用 **OH 自己的 clang** 编成 `.a`。
3. **上游不动**：`3rdparty/`、`deploy/` 之外的 t-mac 上游文件不改；我们的东西集中在 `ohos/sa/`。
4. **不匹配的模型不能打死 SA**：加载前先过 `gguf_admission.cc` 的准入检查。
5. **每个结论都要有证据文件**：`ohos/sa/evidence/NN-*.txt`，并在账本里写清"真因 → 修法"。

---

## 二、阶段计划

| 阶段 | 内容 | 状态 | 判据/证据 |
|---|---|---|---|
| **S0** | 基底清理：目录收敛、引用不悬空、单一进度源（本文件） | ✅ 2026-10-05 | 桌面 719 MB → 404 MB；悬空引用 0 |
| **S1** | STA-1 稳定性：6/6 轮冷启动全绿 | ✅ | `evidence/40-sa-stability-6rounds.txt` |
| **S2** | STA-2 接口鲁棒性：`--stress` bad=0 | ✅ | `evidence/41-sa-robustness-sta2.txt` |
| **S3** | STA-3 真推理：SA 内真 `LoadModel`/`Generate` | ✅ | `evidence/42-sa-real-inference-sta3.txt` |
| **S4** | **t-mac 2bit 模型可用**：LUT 内核/kcfg 与模型形状对齐 | ✅ **达成**：SA 内 `LoadModel`+`Generate` 出真实文本、可复现、失败项 0 | `evidence/45-sa-tmac-kcfg-pair-fix.txt` + FIX-59/60/62/63 |
| **S5** | STA-4 对外可调：权限模型 + 配额 + 第三方接入示例 | ✅ **收口**：S5-1 准入实测（evidence/47）、S5-2 配额实测（evidence/48）、接入文档 [`ohos/sa/INTEGRATION.md`](ohos/sa/INTEGRATION.md) | 账本「S5-1 / S5-2」两节 |
| **S6** | 系统级执行器：意图 → 结构化动作 → 执行 | ✅ **完全收口**：动作层/意图层/两道门全绿；**模型路径链路全通**，分类不可用已由**模型原文**证定为"质量原因"（evidence/58 + FIX-80）：动作层（结构化动作 JSON，evidence/51）+ 意图层（动作表/两道门/双路径，evidence/52+55）全部实测；**模型分类路径实测不可用**（如实回退 keyword，记为已知限制）；t-mac 推理回归通过（evidence/55） | `evidence/51/52/55` + FIX-70~76 |
| **S7** | GUI Agent（愿景，暂不排期）：无障碍读屏 + 输入注入 | ⏳ | 感知-决策-执行闭环 |

---

## 三、当前状态板

**Now（这一轮要做的一件事）**

- **S7-2-A（第一个最小 Agent Loop）✅ 已完成**（`evidence/71`）—— **SA 进程内的第一次完整闭环**
  goal="unlock screen"，2107ms，`VERIFY=PASS`。六段全部有真数据：
  observe(ReadScreen 快照) → digest(文本/元素/box) → decide(规则命中锁屏 Swiper) →
  policy(端点必须在依据元素 box 内) → act(进程内 MMI 注入 9 事件, ret=0) → verify(再读屏对比 → PASS)
  · **坐标全部来自感知**：swipe from/to 取自 Swiper#27 的 box=[368,178,655,629]（511,573）→（511,234），无硬编码
  · **两次如实 FAIL 先于 PASS**：① 空屏（SA 启动早于锁屏）→ `fail(not_lock_screen)`；
    ② 快照 40 节点预算+深度优先走不到 Swiper → `fail(no_swipeable_area)` → 改**广度优先+200 预算**后通过
  · 改动范围：新增 `ohos/sa/agent/`（6 模块）+ `lut_screen` 加结构化快照（遍历改 BFS）+
    `lut_sa_ability` 的 OnStart 挂触发线程（**仅当 goal 文件存在**）；**未新增 IPC、未接 LLM、
    未动 lut_sa.cpp / engine / 3rdparty**；SA 既有 9 方法行为不变

- **S7-2-0（无障碍动作可行性验证）✅ 已完成**（`evidence/62..68`；跨轮汇总见 `evidence/69-s7-2-0-conclusion.txt`）
  四条执行通道的实测结论（全部真错误码，不猜）：
  · **输入注入 `uitest uiInput` = ✅ 唯一可用**：解锁 swipe、下拉通知栏、收起通知栏 三次独立实验
    都真的改变了界面（窗口列表 + 节点/文本计数变化）
  · **a11y 元素动作 `ExecuteAction`（click/back/scroll）= ❌ 受理但不生效**：两次不同界面 × 3 种动作
    全部 `ret=0` 且零变化 → **ret=0 不能当成功**，判据只能是"界面确实变化"
  · **a11y 手势 `InjectGesture` = ❌ 4006 `RET_ERR_NO_CAPABILITY`**（该通道无 gesture 能力登记）
  · **拉起 Ability（aa/AMS）= ❌ 本环境不可用**：`10104001`（ability 不存在）/`10103601`（bundleName 不存在），
    `bm dump -a` 亦报参数错；**注意 `aa` 的 shell 退出码是 0，必须看它打印的 Error Code**
  → **设计调整（已确认）**：① executor 定为**坐标注入**，坐标来自**刚读到的元素 box 中心**（感知驱动，非盲猜）；
    ② a11y 元素动作降为"待判别备选"（先试 `SetTargetBundleName` 再 ExecuteAction）；
    ③ **第一个闭环的目标从"打开设置"改为本环境可用的动作**（解锁 / 通知栏展开收起），先把 Agent Loop 跑通 —— 即 S7-2-A。

- **Phase 3 = S7-1b ✅ 已完成**（`evidence/61`，SA 的 `ReadScreen`，感知方向，独立 IDL 方法）
  判据（全是跨 IPC 的真返回值）：
  1. **准入双证明**：白名单不含调用方 → **201**；恢复默认档位 → **ErrCode=0**（与其它方法同一套门）；
  2. **拿到带文本的元素树**：`{"ok":1,"connected":1,"user":100,...}`，
     `counts={"nodes":13,"withText":7,"clickable":6}`，(win,a11yId) 键全部唯一；
     文本 = `上滑解锁 / 23:17 / 2026年10月5日 / 星期一 / 丙午年八月廿五 / 没有 SIM 卡 / 100%`（屏幕=锁屏）；
     另有 6 个可点元素（Stack/Swiper/SwiperIndicator + box）给 S7-3 用；
  3. **有界性**：budget=8 → `nodes=8`、`truncated=1`（不会把整棵树甩过 IPC）；
  4. **延迟**：首调 **677 ms**（含 RegisterAbilityListener + Connect + 等 channel 回调），后续 **29–32 ms**；
  5. **隐私已落地**：SA 日志只记计数与耗时（`nodes=13 withText=7 clickable=6 ms=31`），**不记屏幕原文**；
     `GetMetrics` 里 `screen=` 只报连接状态；本轮**未调用任何注入类 API**（`ExecuteAction`/`InjectGesture` 留到 S7-3）。
  - 权限机制：`lut_sa.cfg` 的 `permission` 字段（与取证服务同一套，FIX-83）——
    **注意** harness 的注入清单原来漏了 `lut_sa.cfg`（导致 SA 侧 1005），已补（FIX-87）。
  - 实现落点：`ilut_sa.h` / `lut_sa_proxy.*` / `lut_sa_stub.cpp` / `lut_sa_ability.h,*.cpp`（`ReadScreen`）
    + 新模块 `component/lut_screen.{h,cpp}`（进程级单例 + 有界遍历 + 只回文本/可点节点）
    + 客户端 `--screen [maxNodes]` + 取证 `[14]` 段。

**Next（排队）**

1. **Phase 4 = S7-2：元素树 + 话语 → 下一步动作 JSON（「理解」与「决定」）**
   - 输入：`ReadScreen` 的 JSON（文本/可点/窗口）+ 用户话语；
   - 输出：严格校验的动作 JSON（`{"action":"click","a11yId":…}` / `{"action":"back"}` / `{"action":"start_ability",…}`），
     **`source` 如实标注**（model/keyword/none）、`model_said` 原样带回（沿用 S6-2 的纪律）；
   - 判据（`evidence/62`）：给"打开设置"→ 决策出 `start_ability com.ohos.settings`；
     给"点 WLAN"这类需要界面的说法 → 决策出带 `a11yId` 的 click；拿不定 → `source=none` 且不动作。
2. **Phase 5 = S7-3/4：执行闭环「打开设置 → 进子页 → 返回」**（`evidence/63`）
   - 执行侧优先用无障碍动作（`ExecuteAction(elementInfo, action)`，system_basic），
     `INJECT_INPUT_EVENT`（system_core）作兜底（已先在 cfg 里声明）；
   - 重复 3 次 + 断网跑，全程只在本机完成（隐私诉求）。
3. S5 第三方接入示例：HAP 侧最小示例 + 文档（native 版已有 `lut_sa_client`）；
4. 工程化：一条命令出镜像（`intree/make_image.sh`）+ 性能基线（TTFT / tok/s / 内存）。

**Blocked / 已知缺口（写清楚，不留暗坑）**

- 引擎加载一份"张量形状与内核不匹配"的模型时，`ggml-tmac` 的 `LOG(FATAL)` 抛出的异常在某些路径上
  仍会 `std::terminate`（跨 C 栈帧）→ 已用准入检查在**加载前**拦住这一类；根治见 S4。
- ~~t-mac 数值路径全 NaN~~ **已解**（FIX-63）：不是 dtype、不是 tokenizer，而是 x86 那份 kcfg 的参数对错了
  （kfactor=8/未定标 vs 模型需要的 kfactor=16/135-50）。已切默认 + 加 `TMAC_KER_OVERRIDE` 旋钮。
- 串口在高负载下会丢行/断行 → 判据尽量取 guest 内文件（`/data/lut_sa/*`、`/data/local/tmp/lut_evidence.txt`），
  停机后从 userdata 镜像里捞。
- 本机没有整机，HarmonyOS 侧只能在模拟器验证；OH 侧只有 QEMU x86_64（arm64 有 staging 但未实测）。
- **无障碍口子的语义边界**（S7-1a 新发现，已写进 `evidence/60` 与 FIX-83/84）：当前走的是系统给原生进程的
  「UITest ability」模式 —— 能力与权限都对，但语义上是测试框架的通道。生产加固方向 = 让本组件成为**正式的无障碍扩展**
  （或由无障碍管理器提供正式的「系统读屏」登记口）；届时只需换登记方式，`ReadScreen` 的上下层接口不变。

---

## 四、常用命令（复制即用）

```bash
# 一次性编译 + 注入 + 冷启动 + 取证 + 打包证据（STA-3 链路）
MSYS_NO_PATHCONV=1 wsl.exe -d ohbuild -u root -- bash <repo>/ohos/sa/intree/sta3_verify.sh
# 只重编引擎（改了 flags / 内核 / kcfg 时）
wsl -d ohbuild -u root -- bash -c 'LUTSA_FORCE_ENGINE=1 bash /src/ohos/vendor/ohemu/lutsa/../build_engine.sh /src/ohos /src/ohos/vendor/ohemu/lutsa'
# 只看 SA 运行期日志（不依赖 guest 脚本跑完；停机后从镜像里捞）
wsl -d ohbuild -u root -- bash -c 'e2fsck -fy /src/ohos/out/x86_64_virt/packages/phone/images/userdata.img; mount -o loop,ro \$IMG /mnt/ud2; cat /mnt/ud2/lut_sa/rt_stderr.txt'
```

**盘上的关键位置**

| 位置 | 说明 |
|---|---|
| `/src/ohos`（WSL distro `ohbuild`） | OpenHarmony 7.0-Release 源码树（构建目录 `out/x86_64_virt`） |
| `/src/ohos/vendor/ohemu/lutsa` | 组件在树里的落点（`install_into_tree.sh` 生成） |
| `/src/ohos/out/x86_64_virt/packages/phone/images/` | `system.img` / `userdata.img`（注入与取证的落点） |
| `D:\ohos-models\` | 模型与 t-mac 转换工具链（`bitnet-3b-tmac.gguf` = 966 MB；Qwen2.5-0.5B = 491 MB） |
| `C:\Users\NJF\Desktop\t-mac\` | 本仓库 + `ohsign/`（签名链）+ `lutsa-evidence/`、`emulator-evidence/`（早期证据） |

---

## 五、纪律（每轮都适用）

1. **一次只推进一件事**，做完要有"可复现的判据"，并把结论写进账本（真因 → 修法）。
2. **不改上游 t-mac 文件**；我们的改动集中在 `ohos/sa/**`（含 `intree/` 脚本与账本）。
3. **删除/覆盖前先看目标**：确认是"可再生"或"已过期"的产物；拿不准就在进度板里写成待办，不要静默删。
4. **性能数字要有出处**：区分"调优内核命中"与"通用回退路径"，不得混用。
5. **提交粒度**：一件事一个 commit，信息写清"现象 → 真因 → 修法 → 证据文件"。

---

---

## 七、S7 执行顺序（**已拍板**：Phase 0 → 5；S7-1 拆成 1a/1b；T1/T2 暂停）

> 两条设计原则（定下来就别违反）：
> ① **感知与执行是两个方向**：`ReadScreen`（世界→Agent）必须是**独立的 IDL 方法**，不塞进 `ExecuteAction`（Agent→世界）；
>    以后要扩 `ReadCamera/ReadAudio/ReadSensor` 也走这条线。
> ② **顺序不许跳**：看到 → 理解 → 决定 → 操作。第一版只做"看"（dump 元素树），不做自动点击 ——
>    读屏失败能定位到"无障碍没起/元素树错/权限不足"，而点击失败要同时排查坐标/注入/权限/页面状态。

| Phase | 内容 | 判据（证据） |
|---|---|---|
| **0 = S6-4** | ✅ **已收口**：`INTEGRATION.md §5` 补成 SDK 文档（意图/动作字段表 + 完整 ArkTS 执行片段 + 失败处理对照）；**正向授权双证明**实测（`[11a]` 放行 0 / `[11b]` 收回授权 201） | `evidence/59` |
| **1 = S7-0 侦察** | ✅ **已完成**：轻依赖路径存在（无 arkcompiler）、API 面已定位、**无障碍服务实测在跑**（`ps` → `accessibility 323 1 …`） | `evidence/59` 的 `[12]` 段 |
| **2 = S7-1a 探针** | **独立工具，不进 SA**：`lut_a11y_dump` 只链接 `accessibility:accessibleability`（轻依赖 ✓ 无 arkcompiler），调 `AccessibleAbilityClient::GetRoot/GetWindows/GetRootByWindow` → 打印元素树 JSON；在 guest 里以 root/init 身份跑 | `evidence/60`：**能打印带文本的元素列表**；若被拒/拿不到 → **如实记下错误码与原因**（探针的意义就是把未知拆开） |
| **3 = S7-1b ReadScreen** | 把探针能力封成 SA 的**独立方法 `ReadScreen`** + 白名单/隐私（屏幕原文默认不落日志） | `evidence/61`：白名单调用方经 IPC 拿到元素树 JSON |
| **4 = S7-2 决策** | 元素树 + 用户话 → **下一步动作 JSON**（严格校验 + `source` 如实 + `model_said` 原文） | `evidence/62` |
| **5 = S7-3/4 执行闭环** | 执行优先走**无障碍动作**（权限 system_basic ✓），`INJECT_INPUT_EVENT`（system_core）备选；闭环目标先选 **"打开设置 → 进子页 → 返回"** | `evidence/63`：**重复 3 次成功** + **断网也成立** |

**暂停项（写明原因，别偷偷做）**：**T1 多会话**（要拆引擎壳的 model/context 两层，属结构性改动；不在 GUI Agent 关键路径上）、
**T2 arm64**（无整机可测 → 只能静态检查，信息量低；留作"boot 被占住时的填充任务"）。

### S7-0 侦察结论（2026-10-05 实测，**别重复侦察**）

| 问题 | 答案 |
|---|---|
| 无障碍模块在哪 | `foundation/barrierfree/accessibility`；**SA id = 801**，`libaccessibleabilityms.z.so`，`run-on-create: True`；镜像里有 `profile/accessibility.json` + `/system/etc/init/accessibility.cfg` |
| 客户端 API | `interfaces/innerkits/aafwk/include/accessible_ability_client.h` → `AccessibleAbilityClient`：`GetRoot(info, systemApi)`、`GetWindows(vector&, systemApi)`、`GetRootByWindow(windowInfo, info)`、`GetRootBatch(vector&)`；另有更轻的 `accessibility_ui_test_ability.h`（`AccessibilityUITestAbility`） |
| 轻依赖路径（关键） | `external_deps = [ "accessibility:accessibleability" ]`（part_name=accessibility）→ 它自己只依赖 `c_utils/e2fsprogs/ffrt/hilog/init/ipc_single/samgr_proxy/json/cesfwk/want` **无 arkcompiler** ✓；⚠ **`accessibility_common` 带 `runtime_core:ani`**，能不带就别带（`common/include` 只用来取头文件） |
| 元素类型 | `common/include/accessibility_element_info.h`（`AccessibilityElementInfo`/`AccessibilityWindowInfo`/`ActionType`/`ACCESSIBILITY_ACTION_*`） |
| 输入注入（Phase 5 备用） | SDK `@ohos.multimodalInput.inputEventClient`；`INJECT_INPUT_EVENT` = **system_core**（三方不可用 ✓ 平台边界）；`INPUT_MONITORING` = system_basic |

## 八、两件"体质"工作（随时可插，按性价比排序）

| # | 内容 | 要点 | 判据 |
|---|---|---|---|
| **T1 多会话** | 共享权重、各自上下文 | 引擎壳拆出 model/context 两层（现在是一体 ✓）；`llama_context` per session + 共享 `llama_model`；会话数配额 + 内存水位（配额框架已有 ✓） | 两会话**交替推理互不串扰**（各自 prompt 各自答）+ 内存/会话数上限生效 |
| **T2 arm64 形态** | 真机路径的可编译性 | 用 `deploy/tuned/aarch64-hf-bitnet-3b/`（kfactor=16/135-50，与 x86 验证过的那族一致 ✓）+ `install_into_tree.sh` 的 arm 分支（已写 ✓） | 静态检查通过（符号/体积）+ **如实标注"未上机实测"** |

## 六、定时任务运行规则（自动化每轮先读这一节）

> 目标：在**不依赖对话上下文**的前提下持续推进；状态只在 `PLAN.md`（本文件）与账本里。

1. **先读**：本文件 §三「当前状态板」→ `ohos/sa/QEMU-DEPLOY.md` 的最后几条 FIX → `ohos/sa/evidence/` 最新文件。
2. **挑一件事做**：优先做 §三 的 **Now**；若 Now 被外部条件卡住（缺工具/需人工决策），
   就做 **Next** 里不依赖它的一件，并在 §三 里写明"A 卡的、改做 B"。
3. **跑起来再说**：能编就编、能冷启动就冷启动；每轮尽量在 20 分钟内拿到一个可判定的结果
   （长构建可只做完"构建 + 静态检查"，把"冷启动取证"留给下一轮，并在状态板里交接清楚）。
4. **留证据**：结果落到 `ohos/sa/evidence/NN-描述.txt`（NN 递增，用 `ls evidence/ | tail` 取号）；
   踩坑写进 `QEMU-DEPLOY.md` 的 FIX 表（现象 → 真因 → 修法）。
5. **更新本文件**：改 §三（Now / Next / Blocked）与 §二 的状态列；把"下一轮该做什么"写成一句可直接执行的话。
6. **提交并推送**：`git add ohos/sa PLAN.md` → commit（现象/真因/修法/证据）→ `git push origin main`。
7. **取证脚本预算纪律（FIX-67/71/79）**：轻活服务（`lut_evidence.sh`）只放"秒级"分段；重活服务（`lut_evidence2.sh`）
   总时长控制在 **~1.5 分钟以内**（每加一段先估时长）；超了就再拆一个服务 —— 服务有时间预算，靠"重排"只能止损。
   判据里"两个 END 都在"必须逐轮检查。
8. **不许**：跳过验证就宣布完成；删掉 evidence 或账本里的历史结论；把未实测的性能数字写成结论；
   在没有新证据的情况下改动 S1–S3 已验收的部分。
8. **一轮结束时的自检**：`git status` 干净、进度板与代码一致、下一件事写得让人能直接照做。
