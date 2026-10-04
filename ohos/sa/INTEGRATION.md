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

- **模型与 LUT 形状表必须成对**：t-mac 2bit 产物（`kfactor=16 / lut_scales_size=135,50` 那族）与标准量化模型不能混用同一套 kcfg；
  换内核用构建期旋钮 `TMAC_KER_OVERRIDE=<dir>`（见 FIX-63）。加载不匹配的模型会**全 NaN**（不会崩，但输出恒定）。
- **单引擎串行**：一次 `Generate` 会占住 SA 直到出完 token（调用方在 IPC 上排队）。多会话并发（共享权重、各自 context）尚未做。
- **文本质量**：本版证明的是"能力通"，不是"效果好"。LUT 2bit 产物的续写质量本来就有限（桌面参考同档）。
- **SELinux**：本项目在 QEMU 环境是 `permissive`（`enforce=0`）。上量产要按 enforcing 配策略：`service_contexts` 里 6901 的类型、
  调用方域对 `sa_lut_sa_service:samgr_class { get }`、以及 SA 读模型文件的 `file` 权限。
- **权限名**：当前按 uid + 白名单控制；`ohos.permission.LUT_SA_INFER` 这类正式权限名尚未注册（进 `PLAN.md` 的 Next）。
