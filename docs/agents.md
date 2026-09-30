# Agents · 工作规范与口径

> 本文合并原 `docs/agents/*`（domain / workflow / deliver / issue-tracker / triage-labels / archive / voice）与 `docs/glossary/terms.md`。
> 赛题 / 方案 / 参考仓 / 队伍等知识见 [`docs/knowledge.md`](knowledge.md)。

## 文档消费规则

探索代码前先读：根 `CONTEXT.md` → `docs/adr/` 中与改动区域相关的 ADR → [`docs/knowledge.md`](knowledge.md)（赛题、方案、参考仓）→ 本文末术语表。文件不存在就**静默跳过**，不要提示缺失、不要主动建议创建。

- 命名领域概念时，用 `CONTEXT.md` / 下方术语表里的词，不要漂移到词表刻意回避的同义词。
- 输出与已有 ADR 冲突：**显式指出**，不要静默覆盖。
- 结构：single-context 仓（无业务 theme 子目录，无 `CONTEXT-MAP.md`，handoff 目录不启用）。

## 工作流

```
GitHub Issue
  → docs/output/report/（调研，可选）
  → docs/output/prd/prd.md（draft → approved）
  → 实施（Issue 跟踪）→ awaiting-review【停】
  → 通过 → commit / commit-history / archive
```

- **PRD 未批准不写功能代码**（T-MAC 上游代码只读分析除外）
- **Review 先于 commit**
- 任务与状态以 GitHub Issues + labels 为准
- **Handoff 停用**（不用 `docs/output/handoff/`）
- Bug 流：发现 → 诊断（根因 + 复现）→ Issue → 修复 → Review → commit（`Closes #N`）

产出直接落 `docs/output/report/`、`docs/output/prd/`，不使用业务 theme 子目录。

## 公开仓库密钥红线（强制）

本仓库**公开**（GitHub: junfennie162-sketch/fandou-t-mac）。以下内容**永远不得提交**：

- `ohos/hap/build-profile.json5` 的**本地版本**（含 `%USERPROFILE%\.ohos\config\...` 绝对路径 +
  DPAPI 加密的 `keyPassword`/`storePassword`）。仓库里只保留 `"signingConfigs": []` 的干净形态。
- 任何 `.p12` / `.p7b` / `.csr` / `.pem` / `.jks` 密钥材料（本机签名材料放在仓库外
  `C:\Users\NJF\.ohos\` 下）。
- 任何机器绑定口令串（`0000001B…` 形态）与本机私有绝对路径。

**双保险已启用**（新克隆需一次性执行第 2 条）：

1. `git update-index --skip-worktree ohos/hap/build-profile.json5`
   —— 本机改动永不进入暂存区（`git add -A` 也拿它没办法）
2. `.githooks/pre-commit` 提交前扫描暂存内容，命中上述模式即**拒绝提交**
   —— 新克隆启用：`git config core.hooksPath .githooks`

需要更新该文件被跟踪的（干净）版本时：先
`git update-index --no-skip-worktree ohos/hap/build-profile.json5`，提交后记得再设回来。

## 交付物

赛程、截止、提交材料口径见 [`docs/knowledge.md` · 赛事简报](knowledge.md#赛事简报)。本仓落盘位置：

| 交付物 | 路径 |
|--------|------|
| 调研 / 赛题解析 | `docs/output/report/` |
| PRD | `docs/output/prd/prd.md` |
| 任务跟踪 | GitHub Issues（`Aafff623/fandou-t-mac`） |
| 演示 PPT / 逐字稿 | `assets/theme/ppt/` · `assets/theme/script/` |
| 演示视频 | `assets/video/` |
| README 说明图 | `assets/images/readme/` |
| 上游备份 / zip | `assets/backup/` |

官方模板与手册：`assets/backup/official-2026/`；队长对照清单：`docs/output/report/phase1-captain-review.md`。

## Issue tracker（GitHub）

Issue 与 PRD 跟踪走 GitHub Issues，全部用 `gh` CLI。Repo：`Aafff623/fandou-t-mac`（以 `git remote -v` / `gh` 在 clone 内推断为准）。

| 操作 | 命令 |
|------|------|
| 建 | `gh issue create --title "..." --body "..."`（多行 body 用 heredoc） |
| 读 | `gh issue view <number> --comments` |
| 列 | `gh issue list --state open --json number,title,body,labels,comments` |
| 评论 | `gh issue comment <number> --body "..."` |
| 加/删标签 | `gh issue edit <number> --add-label "..."` / `--remove-label "..."` |
| 关闭 | `gh issue close <number> --comment "..."` |

PRD 正文定稿仍可落盘 `docs/output/prd/`，跟踪状态以 GitHub Issue 为准。skill 说「publish to the issue tracker」= 建 GitHub issue；说「fetch the relevant ticket」= `gh issue view <number> --comments`。

## Triage labels

五类 canonical 角色，GitHub label 默认同名；首次用前可 `gh label create <name>` 建：

| Label | 含义 |
|-------|------|
| `needs-triage` | 维护者需评估此 issue |
| `needs-info` | 等 reporter 补充信息 |
| `ready-for-agent` | 已完整规格，可交 AFK agent |
| `ready-for-human` | 需人工实现 |
| `wontfix` | 不处理 |

## 归档

| 何时 | 做什么 |
|------|--------|
| 任务 Review 通过并 commit 后 | 关闭对应 GitHub Issue；摘要写入 `docs/commit-history/` |
| 赛段结束 | 该阶段报告/演示定稿复制到 `assets/backup/`（只读备份） |
| 对话临时稿吸收完毕 | 源临时文件可删或移入 backup；知识正文保留在 [`docs/knowledge.md`](knowledge.md) |

禁止把密钥、账号、未公开设备序列号写入 archive。

## 语气与回答格式

人格：Personal Hermes Engineer（全局 skill `humanizer-output-style`）——中文为主、工程师口吻、有判断、低噪音，称呼用户可用「小 A」。回答格式遵守项目级 Cursor rule [`.cursor/rules/answer-format.mdc`](../.cursor/rules/answer-format.mdc)：

| 要求 | 说明 |
|------|------|
| 结构 | **先简述，再详细回答**（两模块，不可颠倒） |
| 载体 | 表格 → Mermaid → 短列表/代码块 → 短段落 |
| 叠加 | `humanizer-output-style` 管人味；本 rule 管结构与密度 |

本项目口径：

- 创新主体是**系统级加速与调度**，不是聊天 UI
- 技术表述优先：LUT / mpGEMM / SystemAbility / TTFT / 能耗与吞吐
- 对赛题评审口吻：创新性、可落地、可演示、测试数据可核对
- 不夸大未实测数据；引用论文 / 公开数字时标明来源

## 术语表

补充 `CONTEXT.md` 未展开的词，冲突时以 `CONTEXT.md` 为准。

| 术语 | 定义 |
|------|------|
| mpGEMM | Mixed-precision General Matrix Multiply，混合精度矩阵乘 |
| dequantization / 反量化 | 将低比特权重扩回高比特再计算；T-MAC 要避开的开销来源 |
| W4A16 等 | Weight 4-bit × Activation 16-bit 等量化配置记法 |
| SAMgr | System Ability Manager，管理 SA 注册与发现 |
| NDK | Native Development Kit，C/C++ 侧能力 |
| HAP | HarmonyOS 应用包；应用赛题常交，本方向以演示系统/源码为主 |
| 茶思屋 / 黄大年茶思屋 | 作品提交平台（公开报道口径） |
| C4-AI | 中国高校计算机大赛——人工智能创意赛 |
| DevEco Studio | 鸿蒙官方 IDE |
| vlut / Vector LUT | T-MAC 式标量 LUT 的向量化演进思路（见 vlut.cpp） |
| TTFT | Time To First Token，首字响应延迟 |
