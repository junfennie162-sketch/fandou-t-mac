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
| **S6** | 系统级执行器：意图 → 结构化动作 → 执行 | ✅ **动作层收口**：SA 产出结构化动作 JSON 实测通过（四变体 `ErrCode=0` + 合法 JSON；未授权 201；契约见 INTEGRATION.md §5）；执行由应用侧完成 | `evidence/51` + FIX-70/71b |
| **S7** | GUI Agent（愿景，暂不排期）：无障碍读屏 + 输入注入 | ⏳ | 感知-决策-执行闭环 |

---

## 三、当前状态板

**Now（这一轮要做的一件事）**

- **S6-2：自然语言 → 结构化动作（"打开设置"这句话，落到 `{action, bundle}`）**
  - **上一轮已收口**（别重做）：动作层已实测（`evidence/51`）：四变体 `ErrCode=0` + 合法 JSON；未授权 201；
    取证已**拆成两个服务**（FIX-71，轻活 `LUT-EV-END` + 重活 `LUT-EV2-END`）。
  - **本轮实现**（新 IDL 方法 `ExecuteIntent([in] String utterance, [out] String result)`）：
    1. **动作集**：先只支持白名单内的 bundle（默认 `com.ohos.settings`），做成一张小表
       （`/data/lut_sa/intents.txt`，每行 `<关键词或说法>=<bundle>[/<ability>]`，缺省内置"打开设置=com.ohos.settings"）。
    2. **两条判定路径，必须如实区分**：
       - **模型路径**：用固定提示模板让本地模型把 utterance 归类（输出受限 JSON），解析成功则
         `source=model`；
       - **关键词兜底**：模型不可用/输出不可解析时，用动作表里的关键词匹配，`source=keyword`；
       - 返回串形如 `intent={"action":"start_ability","bundle":"com.ohos.settings","source":"model|keyword"}`
         —— **不许把兜底说成模型判断**（对齐账本纪律"区分命中与回退"）。
    3. 命中白名单 → 复用现有 `ExecuteAction` 的产出（同一套 JSON 契约）；未命中 → `201` + 理由。
  - **判据（下一轮冷启动，`evidence/52`）**：`ExecuteIntent("打开设置")` → `ErrCode=0` 且 JSON 里
    `bundle=com.ohos.settings`（`source` 字段如实）；`ExecuteIntent("打开相机")` → 201/拒绝；
    两个 `LUT-EV*-END` 都在；`[7]/[8]/[9]`、`bad=0`、t-mac `LoadModel` 0 无回退。

**Next（排队）**

1. S5 权限模型：`service_contexts` + 调用方白名单（uid / token / 权限名）+ 会话与内存配额（拒绝而非硬扛）；
2. S5 第三方接入示例：一个独立进程的最小客户端（native 版已有 `lut_sa_client`，再给一份 HAP 侧示例）+ 文档；
3. S6 系统级执行器：SA 增加 `ExecuteIntent`（自然语言 → 动作 JSON → `StartAbility`/`Want`），
   **只开放安全动作集**（打开/切换/查询），先不做像素级操控；
4. 工程化：一条命令出镜像（把 `sta3_verify.sh` 的注入部分抽成 `intree/make_image.sh`）+ 性能基线（TTFT/tok/s/内存）。

**Blocked / 已知缺口（写清楚，不留暗坑）**

- 引擎加载一份"张量形状与内核不匹配"的模型时，`ggml-tmac` 的 `LOG(FATAL)` 抛出的异常在某些路径上
  仍会 `std::terminate`（跨 C 栈帧）→ 已用准入检查在**加载前**拦住这一类；根治见 S4。
- ~~t-mac 数值路径全 NaN~~ **已解**（FIX-63）：不是 dtype、不是 tokenizer，而是 x86 那份 kcfg 的参数对错了
  （kfactor=8/未定标 vs 模型需要的 kfactor=16/135-50）。已切默认 + 加 `TMAC_KER_OVERRIDE` 旋钮。
- 串口在高负载下会丢行/断行 → 判据尽量取 guest 内文件（`/data/lut_sa/*`、`/data/local/tmp/lut_evidence.txt`），
  停机后从 userdata 镜像里捞。
- 本机没有整机，HarmonyOS 侧只能在模拟器验证；OH 侧只有 QEMU x86_64（arm64 有 staging 但未实测）。

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
7. **不许**：跳过验证就宣布完成；删掉 evidence 或账本里的历史结论；把未实测的性能数字写成结论；
   在没有新证据的情况下改动 S1–S3 已验收的部分。
8. **一轮结束时的自检**：`git status` 干净、进度板与代码一致、下一件事写得让人能直接照做。
