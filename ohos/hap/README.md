# LUT-SA · HAP 演示工程（T-MAC on HarmonyOS）

把 T-MAC 的比特级查找表（LUT）推理内核装进鸿蒙 HAP：ArkTS 界面 → NAPI → LUT 内核，真机/模拟器一键运行。

> 这一段是"在鸿蒙系统里真的跑起来"的形态；SystemAbility 版本还在 `ohos/sa/`（需要开发板）。

## 已验证状态（2026-09-25，无需 DevEco）

用 OpenHarmony SDK 6.1-LTS 的工具链**直接构建过两个架构的 native 库**（与 DevEco 内部行为一致）：

| 目标 | 内核来源 | 产物 | 编译 | 运行验证 |
|---|---|---|---|---|
| x86_64（Windows 模拟器） | `deploy/tuned`（AVX2, m256 k4096 b4） | `libtmac_hap.so` x86-64 | ✅ | ✅ 已在 WSL 跑通同源内核（`ohos/selftest/main_x86.cpp`） |
| arm64-v8a（真机/arm 模拟器） | `deploy/tuned/aarch64-hf-bitnet-3b`（NEON, m128 k3200 b2） | `libtmac_hap.so` AArch64 | ✅ | ✅ 已用 qemu 跑通同源内核（selftest / sa_smoke） |

复现构建（可选，验证用）：

```bash
# x86_64
cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=<sdk>/native/build/cmake/ohos.toolchain.cmake \
  -DOHOS_ARCH=x86_64 -DOHOS_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
  ohos/hap/entry/src/main/cpp
# arm64-v8a：把 -DOHOS_ARCH 换成 arm64-v8a
```

## 你需要做什么（装 DevEco → 跑模拟器）

1. **安装 DevEco Studio**（Windows 版，官网 developer.huawei.com → DevEco Studio 下载），安装时让它装好默认 SDK。
   - 建议 5.0 及以上版本；打不开工程先看下面"故障排查"。
2. **登录华为账号**（免费注册）。DevEco 首次启动会要求登录，模拟器需要账号授权。
3. **打开工程**：`File → Open`，选择本目录（`ohos/hap`），等待右上角 Sync 完成。
4. **配置签名**（真机需要，模拟器也建议配）：
   `File → Project Structure → Signing Configs → 勾选 Automatically generate signature`，登录账号后自动完成。
5. **创建模拟器**：`Device Manager（设备管理器）→ Local Emulator → New Emulator → 选 Phone，镜像选 x86_64，下载并启动`。
   - 需要电脑开启虚拟化（Windows 功能里的 Hyper-V / 虚拟机平台）。
6. **运行**：顶部运行目标选中模拟器 → 点 ▶ Run 'entry'。
7. **预期结果**：应用界面出现两个按钮——
   - 点「运行内核自测」→ 显示 `✅ 内核自测通过 · PASS: kernel ran, zero-in => zero-out`
   - 点「性能测试」→ 显示 100 次调用总耗时与平均耗时（模拟器性能仅供演示，勿当实测数据）

## 工程结构

```
hap/
├── AppScope/                     # 应用级配置（包名 com.fandou.lutsa）
└── entry/
    └── src/main/
        ├── ets/
        │   ├── entryability/EntryAbility.ets   # 应用入口
        │   └── pages/Index.ets                 # 演示界面（调用 NAPI）
        ├── cpp/
        │   ├── CMakeLists.txt                  # ★ 架构自适应：x86_64→AVX2 / arm64→NEON
        │   ├── napi_init.cpp                   # ★ NAPI 桥（selfTest / bench）
        │   └── types/libtmac_hap/index.d.ts    # ArkTS 类型声明
        └── resources/                          # 图标、字符串、颜色
```

内核源码不在本目录：CMakeLists 通过相对路径引用仓库的 `deploy/tuned/**`（保持单一数据源，不复制代码）。

## 接口

| NAPI | 说明 | 返回 |
|---|---|---|
| `selfTest()` | 零输入冒烟：跑一次内核，校验零入零出 | 状态字符串（含 `PASS`/`FAIL`） |
| `bench(steps)` | 重复调用并计时（1–10000 次） | 形状 / 总耗时 / 平均耗时字符串 |

```typescript
import tmac from 'libtmac_hap.so';
const r = tmac.selfTest();   // "shape m=256 k=4096 n=1 b=4 | PASS: ..."
```

## 故障排查

| 现象 | 处理 |
|---|---|
| 打开工程提示 SDK 版本不匹配 | 改 `build-profile.json5` 里的 `compatibleSdkVersion`（如 `"5.0.0(12)"` → 你本地 SDK 对应版本），或在 DevEco 提示里一键切换 |
| Sync 报签名错误 | 先跳过签名跑模拟器；真机再配 Automatic signing |
| 模拟器装不上 / 启动失败 | 确认 BIOS 虚拟化开启；Windows 功能里启用"虚拟机平台"与 Hyper-V；换个镜像版本重试 |
| 运行时报 `libtmac_hap.so` 加载失败 | 确认模拟器架构是 x86_64（对应 `abiFilters`）；arm 模拟器/真机走 arm64-v8a |
| native 编译报找不到 `deploy/tuned/...` | 本工程假定放在完整仓库里（`ohos/hap` 不能单独拿出仓库编译） |

## 注意

- 模拟器（x86_64/AVX2）与真机（arm64/NEON）是**两套内核代码路径**，报告里请分开表述。
- 模拟器性能数字只能演示流程，不能作为实测数据；真机数据建议在开发板/手机上补测。
