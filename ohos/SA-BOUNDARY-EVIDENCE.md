# 系统服务化形态 · 部署载体与权限边界实测（证据）

> 设备：官方模拟器 Pura X View（HarmonyOS 7.0.0 / API 26，x86_64，4 vCPU）
> 工具：`hdc`（DevEco SDK 26.0.0）
> 日期：2026-09-30 · 结论：**零售版 HarmonyOS（模拟器/真机）不允许第三方注册系统服务**，载体应为开源鸿蒙标准系统镜像/开发板

## 一、原始探测记录

### 1. 身份：非 root，无提权通道

```
$ hdc shell id
uid=2000(shell) gid=2000(shell) groups=2000(shell),1006(file_manager),1007(log),1097(netsys_socket),3009(readproc) context=u:r:sh:s0

$ hdc shell "which su; ls -la /system/bin/su"
/bin/sh: su: inaccessible or not found
ls: /system/bin/su: No such file or directory
```

### 2. SELinux：Enforcing（非宽松）

```
$ hdc shell getenforce
Enforcing
```

### 3. 系统分区：只读，且对 shell 不可访问

```
$ hdc shell "ls -la / | grep -E 'system|vendor|sys_prod|chip_prod'"
drwxr-xr-x   2 root   shell   4096 chip_prod
drwxr-xr-x   9 root   root    4096 sys_prod
d?????????   ? ?      ?          ? eng_system      ← stat 被拒
d?????????   ? ?      ?          ? system          ← stat 被拒

$ hdc shell "test -w /system && echo SYSTEM_WRITABLE || echo SYSTEM_READONLY; touch /system/__probe"
SYSTEM_READONLY
touch: '/system/__probe': Read-only file system

$ hdc shell "ls /system/profile"          # 第三方 SA 注册描述必须落地处
ls: /system/profile: Permission denied

$ hdc shell "ls /system/etc/init"          # SA 启动配置必须落地处
ls: /system/etc/init: Permission denied

$ hdc shell "mount | grep -E ' /(vendor|sys_prod|data) '"
/dev/block/vdb on /vendor   type ext4 (ro,seclabel,relatime)
/dev/block/vdc on /sys_prod type ext4 (rw,seclabel,relatime)   ← 唯一可写“系统类”分区
/dev/block/vdd on /data     type ext4 (rw,...)

$ hdc shell "test -w /sys_prod && echo W || echo N; ls -ld /sys_prod /sys_prod/etc"
N
drwxr-xr-x 9 root root 4096 /sys_prod
ls: /sys_prod/etc: Permission denied
```

### 4. samgr 机制本身是活的（机制可用，槽位对第三方关闭）

```
$ hdc shell "hidumper -ls"
System ability list:
SystemAbilityManager   RenderService   AbilityManagerService   DataObserverMgr
AgentManagerService    AccountMgr      BundleMgr               FormMgr
ApplicationManagerService  Installd   AccessibilityManagerService ...
（共 100+ 个系统 SA；含 SystemAbilityManager=samgr 本体）

尾部为 6xxxx/7xxxx 动态编号项（应用侧注册的应用级服务），
无任何第三方系统 SA 槽位；本项目规划的 SA_ID=6901 不存在。
```

## 二、结论（三重锁）

| 锁 | 实测 | 影响 |
|---|---|---|
| 身份 | `uid=2000(shell)`，无 `su`、无 root | 无法执行系统级写操作/注册 |
| SELinux | `Enforcing` | 即使有文件，无对应 `.te` 策略的进程也会被拒 |
| 系统分区 | `/system` 只读且 shell 不可 stat；`/system/profile`、`/system/etc/init` Permission denied；`/sys_prod` 对 shell 不可写 | 注册描述（profile.json）与启动配置（init cfg）无法落地 |

→ **第三方应用在零售版 HarmonyOS 上注册系统服务（SystemAbility）在机制上被关闭**，与代码完成度无关。

## 三、我们已完成 vs 载体缺口

| 项 | 状态 |
|---|---|
| SA 业务内核（CreateSession / LoadModel / PrepareWorkspace / WarmKernel / InferTokenBatch / ReleaseSession） | ✅ 已实现（`ohos/sa/`，约 800 行） |
| 注册描述 | ✅ `profile.json`（LutSystemAbility / libtmac_sa.z.so / run-on startup）+ `lut_sa.cfg`（class core / SA_ID=6901） |
| 感知 → QoS 策略 | ✅ `ohos/sched/qos_policy.cpp` |
| 全链路冒烟 | ✅ 标准系统目标（aarch64-linux-ohos）qemu 实测 PASS（Create→Load→Prepare→Infer→Release + 前后台信号切换） |
| **samgr 绑定 + 跨进程 IPC** | ❌ 需把 SA 转成 OH 子系统组件（GN 构建 + bundle.json + sepolicy）并**编入系统镜像**——载体工作，非算法工作 |
| 可演示的替代形态 | 应用级：已交付 HAP（本提交）；应用内服务化近似：ServiceExtensionAbility（未实现，1–2 天，零售设备可用） |

## 四、真机复核（待补）

零售真机（HBN-AL00）插上后执行同一套探测，预期同为 Enforcing + 系统分区不可写：

```powershell
hdc shell id; hdc shell getenforce
hdc shell "test -w /system && echo W || echo R; ls /system/profile"
hdc shell "hidumper -ls | head -30"
```
