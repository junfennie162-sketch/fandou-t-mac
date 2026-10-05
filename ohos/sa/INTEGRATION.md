# 接入 LUT-SA SystemAbility（SA_ID 6901）

> 这份文档面向**系统集成方**：把 LUT-SA 作为系统能力并入你自己的 OpenHarmony 镜像，或者在同镜像内以系统组件身份调用它。
> 相关的实测证据在 [`evidence/`](evidence/)，踩坑账本在 [`QEMU-DEPLOY.md`](QEMU-DEPLOY.md)，总计划在仓库根 [`PLAN.md`](../../PLAN.md)。

---

## 0. 先读这一段：平台边界（不写清楚就是误导）

| 场景 | 能不能直接调 6901 | 依据 |
|---|---|---|
| **同镜像内的系统组件 / 系统应用 / 原生进程**（uid 0 / 1000 / 2000 / OH 内部服务） | ✅ 可以（本仓库的 `lut_sa_client` 就是这么调的） | `evidence/47` 实测：`GetSystemAbility(6901)` 成功、6 个方法可调 |
| **零售 HarmonyOS 上的第三方 App** | ❌ **不能** | 三方不能注册 SA；`AppServiceExtensionAbility` 属特权扩展（`not allow use privilege extension`，见 FIX-51）；也不保证拿得到系统 SA 的 IDL proxy |
| **OEM / 合作伙伴** | ✅ 以"系统集成"方式并入镜像（预置进产品配置 / 预装清单） | 正确载体是**自己构建的 OpenHarmony 标准系统镜像**，见 FIX-50 的两条路线 |

**结论**：LUT-SA 是**系统厂商侧**的能力。三方应用将来要用，正路是系统侧再包一层官方出口（能力框架 / Intent），而不是直接拿 proxy —
这属于后续档位（`PLAN.md` 的 S6/S7），本版不承诺。

---

## 1. 前置条件（准入与配额，三个文件都在 `/data/lut_sa/`，**改完不用重启 SA**）

| 旋钮 | 内容 | 默认 |
|---|---|---|
| 调用方准入 | 默认档位：uid `0`/`1000`/`2000` 或 `<10000` 可用全部方法；其余 uid 只能调只读方法（`NativeVersion`/`GetMetrics`） | 默认档位 |
| `/data/lut_sa/allow_uids.txt` | 每行一个 uid；**存在且非空 → 白名单模式**（只认表里的 uid 调推理类方法） | 不存在 |
| `/data/lut_sa/quota.txt` | `model_mb=2048`：单次可加载模型大小上限（`<=0` = 不限制） | 2048 MB |

模型文件建议放 **userdata**（例：`/data/local/tmp/model.gguf`）；不要塞进 `system.img`（2 GB 分区装不下 966 MB 的大模型）。
注入镜像时把文件的 SELinux 标签设成 `u:object_r:system_file:s0`（宿主侧写 xattr 即可），否则 enforcing 模式下 SA 打不开。

---

## 2. 最小调用样例（native C++）

完整可编译的版本就是 [`component/lut_sa_client.cpp`](component/lut_sa_client.cpp)（本仓库的取证客户端）。复用它只要三步：

1. 把 [`component/idl/`](component/idl/) 下的生成物（`ilut_sa.h` / `lut_sa_proxy.cpp/.h` / `lut_sa_stub.cpp/.h`）拷进你的组件；
2. `BUILD.gn` 里加 `external_deps = [ "samgr:samgr_proxy", "ipc:ipc_core", "c_utils:utils", "hilog:libhilog" ]`；
3. 按下面的骨架调用：

```cpp
#include "iservice_registry.h"
#include "if_system_ability_manager.h"
#include "iremote_object.h"
#include "ilut_sa.h"
#include "lut_sa_proxy.h"

using namespace OHOS;

sptr<ISystemAbilityManager> samgr =
    SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
sptr<IRemoteObject> remote = samgr->GetSystemAbility(6901);   // 失败 → SA 没起来/权限不足
sptr<ILutSa> sa = iface_cast<ILutSa>(remote);
if (sa == nullptr) { /* 取不到 proxy：报错退出，不要空转 */ }

std::string r;
ErrCode e = sa->NativeVersion(r);          // 探活（只读，不需要准入）
e = sa->LoadModel("/data/local/tmp/model.gguf", 4, 512);   // 准入 + 配额在这里生效
if (e != 0) { /* 见下面错误码表 */ }
e = sa->Generate("The capital of France is", 24, 0.8, 40, r);   // 真推理，返回文本
e = sa->GetMetrics(r);                     // 含 caller/policy/quota/engine 状态，排障先看这行
e = sa->Release(r);                        // 归还模型与 KV 内存
```

**错误码表**（客户端只需要认这几个）：

| 码 | 含义 | 处置 |
|---|---|---|
| `0` | 成功 | —— |
| `22` (EINVAL) | 参数/配额不满足：模型路径不可读、超 `model_mb` 上限、未加载就推理前置条件不满足 | 看 `GetMetrics` 的 `quota:` 与 `engine:` 段 |
| `201` | 准入拒绝：调用方 uid 不在档位/白名单内 | 找系统侧加白名单，或改由系统组件代调 |
| `29189` | 远端对象死亡（SA 被重启，例如加载了不匹配的模型） | 重取 proxy 再试；`GetMetrics` 的 `diag:` 段有原因 |

---

## 3. 排障：三条通道（按可用性排序）

1. **`GetMetrics`**（能通的通道）：一次调用就能看到 `caller: uid=… token=… last_method=… | policy: … | quota: … | engine=ready/absent n_ctx=… | diag: … | llama_log: …`。
2. **`/data/lut_sa/rt_stderr.txt`**：SA 进程的 stderr 落盘（引擎的 `[shim]` 阶段标记、`GGML_ASSERT`、`LOG(FATAL)` 都在这里）。
   在 QEMU 环境里这是**唯一**能看到"引擎为什么失败"的通道（hilog 抓不到 SA 的日志、hdc 也被 watchdog 杀）。
3. **串口**（`console=ttyS0`）：本仓库的取证脚本 `/system/bin/lut_evidence.sh` 会把 guest 内的调用结果整体打到串口。

---

## 4. 已知限制（避免踩同一批坑）

- **意图的"模型分类"路径当前不可用（如实记录）**：`ExecuteIntent` 会先让本地模型按模板输出受限 JSON，
  且**严格校验**（输出里必须出现动作表内的 bundle 才认）。实测（`evidence/55`）：模型已加载时该路径**确实被走过**，
  但 t-mac 2bit 产物的输出没有通过校验 → **如实回退到关键词表**（`source=keyword`）。
  **实测原文（`evidence/58` 的 `[1d3]`）**：同一引擎下 `model_said=" notes RotTV abroadTC foreverDATA given Resolnih absolute assumptionena…"`
  —— **模型输出是词沙拉**，既不是 JSON 也不含任何包名；**引擎跨 IPC 调用存活**（`[1d2]` 另起进程读指标仍 `engine=ready`）
  → 所以这是"**模型质量**"问题而非"链路"问题：链路（加载/存活/推理/严格校验/回退）全部工作正常。
  本版**不放宽校验**（宁可回退，也不接受模型随口给的包名）。
  要启用需换更强的模型或加约束解码 —— 判据是 `GetMetrics`/结果串里的 `model_said="…"`（S6-3 起会带出模型原始输出）。

- **模型与 LUT 形状表必须成对**：t-mac 2bit 产物（`kfactor=16 / lut_scales_size=135,50` 那族）与标准量化模型不能混用同一套 kcfg；
  换内核用构建期旋钮 `TMAC_KER_OVERRIDE=<dir>`（见 FIX-63）。加载不匹配的模型会**全 NaN**（不会崩，但输出恒定）。
- **单引擎串行**：一次 `Generate` 会占住 SA 直到出完 token（调用方在 IPC 上排队）。多会话并发（共享权重、各自 context）尚未做。
- **文本质量**：本版证明的是"能力通"，不是"效果好"。LUT 2bit 产物的续写质量本来就有限（桌面参考同档）。
- **SELinux**：本项目在 QEMU 环境是 `permissive`（`enforce=0`）。上量产要按 enforcing 配策略：`service_contexts` 里 6901 的类型、
  调用方域对 `sa_lut_sa_service:samgr_class { get }`、以及 SA 读模型文件的 `file` 权限。
- **权限名**：当前按 uid + 白名单控制；`ohos.permission.LUT_SA_INFER` 这类正式权限名尚未注册（进 `PLAN.md` 的 Next）。

---

## 5. 系统级动作（S6）：SA 产出「结构化动作」，应用侧执行

**为什么要这样分工（实测结论）**：SA 侧直连 AMS（Raw IPC，code 1001）**机制是通的** ——
AMS 会收下我们的 parcel 并回一个真实错误码；但三种参数组合（只给 bundle / 显式 ability / 再加 userId=100）
**一律回 `22`**（参数校验级拒绝）→ 在 system-caller 语境下 AMS 不接受这次启动（见账本 FIX-69/70）。
所以本版把职责切干净：

| 谁 | 干什么 |
|---|---|
| **SA（6901）** | 准入（Tier-A/白名单）+ **动作白名单**（`/data/lut_sa/actions_allow.txt`）+ 产出**结构化动作 JSON** |
| **应用侧**（有完整 ability kit 与自身身份） | 拿 JSON 去 `startAbility` 执行，并把结果回报 |

### 调用与返回

```cpp
std::string r;
sa->ExecuteAction("start_ability", "com.ohos.settings/com.ohos.settings.MainAbility@100#phone", r);
// r 形如：
// action={"action":"start_ability","bundle":"com.ohos.settings","ability":"com.ohos.settings.MainAbility",
//         "module":"phone","userId":100,"ams_try":22}  ams=22
```

- **arg 语法**：`<bundle>[/<ability>][@<userId>][#<module>]`
  ⚠ 在 shell 里调用时 `#` 要加引号（`'…@100#phone'`），否则被当注释（FIX-70 踩过）。
- **返回码**：`0` = SA 侧职责完成（**策略通过 + 动作已产出**）；`201` = 准入/白名单拒绝。
  `ams_try` 字段是 SA **直连 AMS 的尝试结果**（本环境恒为 22，如实带回，不美化）。
- **应用侧执行的参考写法**（HAP 侧，ArkTS）：

```ts
// 收到 SA 的动作 JSON 后
let action = JSON.parse(actionJson);
if (action.action === 'start_ability') {
  let want: Want = { bundleName: action.bundle,
                     abilityName: action.ability,
                     moduleName: action.module };
  this.context.startAbility(want)
    .then(() => { /* 回报成功 */ })
    .catch((e: BusinessError) => { /* 回报失败码，别吞掉 */ });
}
```
- **自定义动作表**（不用改代码就能扩）：`/data/lut_sa/intents.txt`，每行 `<说法>=<bundle>[/<ability>]`：

```
打开设置=com.ohos.settings
看相机=com.ohos.camera
开会=com.ohos.calendar/EntryAbility
```
  表里能命中**不等于**允许执行：`com.ohos.camera` 这类还要写进 `/data/lut_sa/actions_allow.txt`（动作白名单），
  否则 `ExecuteAction` 会回 **201**（实测见 `[10c]/[10e]`）。内置默认表已含设置/相机/联系人/音乐，
  但**默认动作白名单只放 `com.ohos.settings`** —— 最小权限。
- **`ExecuteIntent` 的返回值**（自然语言那一层）：

```
intent={"utterance":"打开设置","bundle":"com.ohos.settings","source":"keyword","model_said":"(engine not ready)"}
       action={"action":"start_ability","bundle":"com.ohos.settings","ability":"","module":"","userId":-1,"ams_try":22} ams=22
```
  即：**前半段是"意图解析结果"，后半段是"动作 JSON"**，两者拼在一个字符串里（`action=` 之后那一段是合法 JSON，可以直接解析）。

### 字段表（给要做"应用侧执行器"的人）

**① 意图段** `intent={...}`

| 字段 | 含义 | 取值 |
|---|---|---|
| `utterance` | 原始那句话 | 字符串 |
| `bundle` | 解析出的目标包名 | 白名单内才非空；没命中为空 |
| `source` | **谁做的判断（如实）** | `model`（模型路径通过严格校验）/ `keyword`（关键词表兜底）/ `none`（没匹配） |
| `model_said` | **模型的原始输出**（清洗+截断 120 字） | 用于复盘"为什么没用模型路径"（实测为词沙拉 → 所以回退） |

**② 动作段** `action={...}`（**这一段是合法 JSON，正式契约就是它**）

| 字段 | 含义 | 说明 |
|---|---|---|
| `action` | 动作类型 | 本版只有 `start_ability` |
| `bundle` | 目标包名 | 已被 SA 的**动作白名单**校验过 |
| `ability` / `module` | 显式启动用（可空） | 来自调用方 `arg` 的 `/<ability>` 与 `#<module>` |
| `userId` | 目标用户 | `-1` = 默认用户 |
| `ams_try` | **SA 直连 AMS 的尝试结果** | 本环境恒 `22`（system-caller 不被接受）；**如实带回，不美化** |

### 应用侧执行器（HAP / ArkTS，**完整片段**）

```ts
import { common, Want, BusinessError } from '@kit.AbilityKit';

// 从 SA 的返回串里取出动作段并执行；executor 必须自己做"第二道门"
export async function runAction(saResult: string, ctx: common.UIAbilityContext): Promise<string> {
  const i = saResult.indexOf('action=');
  if (i < 0) return 'FAIL: no action segment';
  let act: { action: string; bundle: string; ability?: string; module?: string; userId?: number };
  try {
    act = JSON.parse(saResult.slice(i + 'action='.length).split(' ams=')[0]);
  } catch (e) {
    return 'FAIL: action json parse error';
  }
  if (act.action !== 'start_ability' || !act.bundle) return 'FAIL: unsupported action';

  // 第二道门：应用侧自己的允许清单（别把"SA 说可以"当成"已经执行了"）
  const ALLOW = ['com.ohos.settings'];           // 按你自己的策略维护
  if (!ALLOW.includes(act.bundle)) return 'DENY: bundle not in app-side allow list';

  const want: Want = { bundleName: act.bundle };
  if (act.ability) want.abilityName = act.ability;
  if (act.module) want.moduleName = act.module;
  try {
    await ctx.startAbility(want);
    return 'OK';
  } catch (e) {
    const err = e as BusinessError;
    return `FAIL: startAbility code=${err.code} msg=${err.message}`;   // 失败也如实回报
  }
}
```

**失败处理对照**（应用侧只需认这几个）：

| 你看到 | 含义 | 怎么办 |
|---|---|---|
| `ErrCode=0` + `action={...}` | SA 侧完成（策略过、动作已产出） | 交给上面的 `runAction` 执行 |
| `ErrCode=201` | 准入或**动作白名单**拒绝 | 找系统侧加白名单；别在应用侧绕过 |
| `ErrCode=22` | 参数/配额问题（如模型超限、显式启动参数不全） | 看 `GetMetrics` 的 `quota:`/`diag:` |
| `ErrCode=29189` | 远端对象死亡（SA 被重启） | 重取 proxy 再试 |
| `action` 段里 `ams_try != 0` | SA 直连 AMS 没成功（**当前环境正常**） | 忽略它，执行交给你（应用侧） |

- **安全边界**：SA 只产出**白名单内**的 bundle；应用侧执行前应**再校验一次**（两道门），
  并把执行结果回报给调用方 —— **不要把"SA 说可以"当成"已经执行了"**。
