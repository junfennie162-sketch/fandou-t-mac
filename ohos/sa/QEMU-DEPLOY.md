# LUT-SA → OpenHarmony 标准系统（QEMU）落地记录

> 目标：把 LUT-SA 从「应用侧 HAP」推进到「**系统组件**」——作为 OpenHarmony 标准系统的
> SystemAbility 编进系统镜像，在 QEMU 里开机点亮（`sa_main` 拉起 → 注册 samgr → `hidumper` 可见）。

## 一、为什么是 7.0-Release + qemu_x86_64_linux_full

| 候选 | 结论 |
|---|---|
| 官方预编译 QEMU 镜像 | **不存在**（4.0/5.0/7.0 镜像目录只有源码包 + dayu200/hispark 板子镜像） |
| 5.0 / 6.0 / 6.1 源码 | `device/qemu/*` 只有 `linux`（迷你系统），**无 `linux_full`**（标准系统） |
| **7.0-Release** | `device/qemu/x86_64_virt/linux_full` + `vendor/ohemu/qemu_x86_64_linux_full` **唯一齐备** |
| 下载方式 | 源码包 76.9 GB（解包 ~180 GB，D 盘放不下）→ 改 **repo sync 浅克隆**（~13 GB，26 MB/s，走宿主代理） |

主机侧：Windows 11（15.7 GB RAM / 24 线程）+ WSL2（`ohbuild` = Ubuntu 22.04，10 GB RAM / 40 GB swap / **KVM 可用**）。

## 二、已完成的工程步骤

1. **WSL 网络**：宿主代理 `127.0.0.1:7892` 在 NAT 模式下不可达 → 用 WSL 网关 IP（`172.31.x.1:7892`）配
   `http(s)_proxy` + `git config --global http.proxy`，gitee/gitcode/github 全通（26 MB/s）。
2. **源码获取**：`repo init -u https://gitcode.com/openharmony/manifest.git -b OpenHarmony-7.0-Release --depth=1`
   （repo 工具本体从 GitHub 官方镜像取；7.0 manifest 共 103 个项目）。
3. **补齐 7.0 manifest 缺失的「产品层」**（`ohos/sa/intree/ensure_tree_deps.sh`，幂等）：
   - `vendor/ohemu`（QEMU 产品配置 + 官方 `qemu_run.sh`）
   - `productdefine`（产品 inherit：`rich.json` / `chipset_common.json`）
   - `build/subsystem_config.json` 注册 `"ohemu": {"path": "vendor/ohemu"}`
4. **LUT-SA 组件并入源码树**（`ohos/sa/intree/vendor/ohemu/lutsa/`，`install_into_tree.sh` 一键安装）：
   - `BUILD.gn`：`ohos_shared_library` → `/system/lib64/libtmac_sa.z.so`；`ohos_prebuilt_etc` → profile + init cfg
   - `bundle.json`：组件 `lutsa` / 子系统 `ohemu`
   - 业务内核全部自包含（会话/工作区/瓦片/QoS），**不依赖预编译引擎** → 任意架构可编
5. **产品配置挂载**：`patch_product.py` 把 `lutsa` 追加进 `qemu_x86_64_linux_full/config.json` 的
   `subsystems`（自动剥注释、幂等）。

## 三、SELinux 口径（重要）

- 生产口径：组件随附独立域策略 `sepolicy/base/te/lut_sa.te`（`type lut_sa, sadomain, domain;`）。
- **首次点亮口径**：7.0 的域声明集中在基线策略 `sepolicy/base/public/type.te`，新增域需改动基线策略；
  为降低首次启动失败面，init cfg 暂用**已有的生产级域** `u:r:compiler_service:s0`
  （同为 `sa_main` 宿主 + dlopen 自有库的 SA，权限集正好覆盖：加载 `/system/lib64`、IPC 到 samgr、hilog）。
  点亮成功后再切回自有域（追加基线 `type` 声明 + allow 规则，只影响策略分区，可单独重编）。

## 四、构建与运行（一条链）

```bash
# 源码树内（WSL: ohbuild 发行版）
bash /mnt/c/.../ohos/sa/intree/ensure_tree_deps.sh /src/ohos      # 补产品层
bash /mnt/c/.../ohos/sa/intree/install_into_tree.sh /src/ohos /mnt/c/.../ohos/sa  # 装组件
cd /src/ohos && bash build/prebuilts_download.sh                  # 预编译件
./build.sh --product-name qemu_x86_64_linux_full --ccache --jobs 6
# 产物：out/x86_64_virt/packages/phone/images/{bzImage,ramdisk.img,system.img,...}
# 启动（官方脚本；KVM 自动启用）：bash vendor/ohemu/qemu_x86_64_linux_full/qemu_run.sh
```

启动参数要点（官方 `qemu_run.sh`）：`q35,accel=kvm`、`-cpu max`、`-m 4096`、
`-kernel bzImage -initrd ramdisk.img`、system/vendor/sys_prod/chip_prod/data 依次挂到 `vdb~vdf`、
`ohos.boot.hardware=virt`、`console=ttyS0,115200`。

## 五、点亮验收（三条证据）

1. `hidumper -ls | grep -i lut` → 出现 `LutSystemAbility (6901)`
2. `hidumper -s 6901` → 打出我们的 banner（`kernel: m128-k3200 LUT (kcfg embedded), 2.44 bit/weight`）
3. `hilog | grep LutSa` → `OnStart (SA_ID=6901)` + `Publish: ok` + `CreateSession: 0`

> 备注：自动化流水线 `/root/pipeline2.sh`（WSL 内）按「等同步 → 补依赖 → 装组件 → prebuilts → 编译」顺序无人值守执行，
> 日志 `/src/{sync,pipeline,prebuilts,build}.log`。
