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
   - `build/subsystem_config.json` 注册 `"ohemu": {"path": "vendor/ohemu/lutsa"}`（**只指向组件目录**，
     不能写 `vendor/ohemu` —— 见 §六 FIX-1）
   - 同时注册 `"product_x86_64_virt" → vendor/ohemu/qemu_x86_64_linux_full`（产品自带 `bundle.json` 声明的子系统）
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
./build.sh --product-name x86_64_virt --ccache --jobs 6
#   ★ 产品名是 config.json 里的 product_name（x86_64_virt），不是目录名（qemu_x86_64_linux_full）
#   ★ 不要加 --no-prebuilt-sdk=true：webview 要链接 prebuilts/ohos-sdk 的 libbundle_ndk.z.so（见 §六 FIX-6）
#   ★ --jobs 6 是给 WSL 10 GB 内存留余地（40 GB swap 兜底）
# 产物：out/x86_64_virt/packages/phone/images/{bzImage,ramdisk.img,system.img,...}
# 启动（官方脚本；KVM 自动启用）：bash vendor/ohemu/qemu_x86_64_linux_full/qemu_run.sh
# 带证据采集：bash ohos/sa/intree/qemu_boot_lutsa.sh daemon（串口落盘 + monitor 抓图 + hdc 8710）
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

## 六、7.0 构建踩坑与修复（已固化进 `ensure_tree_deps.sh`，幂等）

首次编译在 LOAD/GN/ninja 三个相位连撞 6 处，全部定位到根因并修复；下表按遇到顺序：

| # | 相位 | 报错原文（节选） | 根因 | 修复 |
|---|---|---|---|---|
| FIX-1 | LOAD | `subsystem name config incorrect in '.../qemu_csky_mini_system_demo/ohos.build', build file subsystem name is product_qemu_csky_mini_system_demo, configured subsystem name is ohemu` | `subsystem_config.json` 里 `ohemu → vendor/ohemu`，hb 递归扫到 15 个产品目录各自 `bundle.json` 声明的子系统名，与 `ohemu` 冲突 | 注册路径收窄为 `vendor/ohemu/lutsa`；**不要**为了躲它去删产品目录（本次已把删掉的 15 个目录原样还原） |
| FIX-2 | LOAD | `TypeError: 'NoneType' object is not subscriptable` @ `subsystem_info.py:52 merge_subsystem_overlay()` | 产品目录下有个手写的 `subsystem_config_overlay.json`（内容只有 `product_x86_64_virt` 一项）。该文件被读入后没有 `subsystem` 段，而上游 hb 在此处用了 `subsystem_config_overlay.get('subsystem').get(...)` → 上游 bug 被触发 | 删除该文件（正确做法是把子系统补进 `build/subsystem_config.json`，不是塞 overlay） |
| FIX-3 | LOAD | `OHOSException: product qemu_x86_64_linux_full@None not found` | **产品名 ≠ 目录名**：`config.json` 里 `"product_name": "x86_64_virt"`，而 `--product-name` 传的是目录名 | 构建命令改用 `--product-name x86_64_virt` |
| FIX-4 | LOAD | `The product use a feature that is not supported by this part, part_name='drivers_peripheral_audio', feature='drivers_peripheral_audio_vendor_alsa_path'` | vendored `vendor/ohemu` 比 7.0 的 `drivers/peripheral/audio` 版本新，多带一个 7.0 驱动不认识的特性（全树无消费者，`device/qemu/common/virt_full/audio_alsa` 已无 BUILD.gn 引用） | 从 `virt_common{,x86_64}.json` 删掉该特性行 |
| FIX-5 | GN | `ERROR at //build/config/components/ets_frontend/ets2abc_config.gni:337:9: OHOS component : (sdk) not found` | 产品配置被削掉了上游的 `thirdparty`（typescript/alsa-lib/pcre2/expat…）与 `sdk` 两个子系统 | 还原为「上游 subsystems + 仅追加 `ohemu:lutsa`」 |
| FIX-6 | ninja | `ninja: error: '.../prebuilts/ohos-sdk/linux/26.0.0/native/sysroot/usr/lib/x86_64-linux-ohos/libbundle_ndk.z.so', needed by 'web/webview/libnweb_ohos_adapt.z.so', missing and no known rule to make it` | 表象：产品构建要链接 NDK 里的 `libbundle_ndk.z.so`。真因在下一条 FIX-7 —— SDK 构建挂了 → 没执行后处理 → `prebuilts/ohos-sdk` 始终为空。**不要**试图用 `--no-prebuilt-sdk=true` 绕（那只会把同一个错误留在原地） | 先修 FIX-7/FIX-8 让 SDK 产物落地；`prebuilts/ohos-sdk/linux/<api>/` 一旦存在，`should_build_sdk()` 会自动跳过 SDK 阶段 |
| FIX-7 | SDK ninja | `/usr/bin/env: 'python': No such file or directory`（一次 136 个目标同时 FAILED，code=127/126） | **WSL 是 Ubuntu 22.04，只有 `python3`，没有 `python`**；而 OH 的构建脚本大量使用 `#!/usr/bin/env python`（如 `build/toolchain/gcc_solink_wrapper.py`、`gen_module_info.py`）。注意：这个错误会伪装成"链接错误"——`gcc_solink_wrapper.py` 起不来时，ninja 打印的是 wrapper 命令本身，容易误判成链接器问题 | `ln -sf /usr/bin/python3 /usr/local/bin/python`（或 `apt install python-is-python3`） |
| FIX-8 | 产品 ninja | `third_party/libtiff` 的 `install.sh` → `./autogen.sh: 12: libtoolize: not found`（aclocal / autoheader / automake / autoconf 全 127） | WSL 是精简镜像，**autotools 全家桶没装**（与 FIX-7 同类：缺系统包，而 hb 的 `tools_checker.py` 不检查这些） | `apt-get install -y autoconf automake libtool libtool-bin m4 bison flex libfl-dev gperf pkg-config libssl-dev libelf-dev zlib1g-dev e2fsprogs dosfstools cpio bc xsltproc libxml2-utils ruby`（`libfl-dev` 是 FIX-12 要的 `FlexLexer.h` 的来源，一并装上） |
| FIX-9 | SDK 续编 | `ld.lld: error: undefined symbol: __aeabi_unwind_cpp_pr0`，目标 `ohos_clang_cortex_m/.../adapter/ohos_clang_cortex_m/libc.so` | **OH 7.0-Release 自身的缺陷**：cortex-m（arm-liteos）的 NDK libc 链接时 `.rsp` 里没有 libunwind，而 `crtn.o` 的 `.ARM.exidx` 需要 EHABI 人格例程。该产物是**迷你系统**用的，与标准系统镜像无关 | 用 `ninja -w dupbuild=warn -k 0 -j N -C out/sdk build_ohos_sdk` 把其余目标编完，再跑 `finish_sdk_prebuilts.sh` 手工做 hb 的 `_post_process_sdk` 搬运（脚本见同目录） |
| FIX-10 | 产品 ninja（arkui） | `FAILED: gen/foundation/arkui/ace_engine/idlize_gen/...`，真因藏在日志里：`npm ERR! TAR_BAD_ARCHIVE: Unrecognized archive format`（`npm install` @ `arkoala_generator`） | **`repo sync --depth=1` 不拉 git-lfs 对象**：`foundation/arkui/ace_engine` 里 6 个 `arkui_idlize/idlizer-*.tgz` 全是 **131 字节的 LFS 指针**（`version https://git-lfs.github.com/spec/v1`），npm 自然解不开。全树另有 **456 个** LFS 指针，分布在 `device/qemu`(10)、`foundation/graphic`(10)、`foundation/multimedia`(39)、`applications/standard`(62)、`base/update`(60)、`device/soc`(239) 等 | 在对应仓库里 `git lfs pull --include="<路径>/*.tgz"`（ace_engine 实测 6 个文件共 ~1.4 MB）。**建议首次 sync 后就全量补 LFS**：见 `ensure_tree_deps.sh` 的 `fix_git_lfs()` |
| FIX-11 | 构建性能（不报错，但慢 10×） | 没有 FAILED、没有 ERROR，但构建龟速（约 18 目标/分钟）；`free -m` 显示 **可用内存 117 MB、Swap 用掉 11.5 GB**，`loadavg` **20.8** | `--jobs 10` 对 WSL 的 9.9 GB 内存太激进：多个重型 C++ TU（arkui / arkcompiler / graphic）同时展开时内存打爆，内核疯狂换页，CPU 全在等 I/O。**这不是编译慢，是内存不够** | 降到 **`--jobs 6`** 重启（ninja 增量续编，只重做在飞的目标）：swap 立刻回落到 ~57 MB、可用内存 7.7 GB、构建恢复满速。**WSL 10 GB 内存下请固定用 `--jobs 6`，不要贪高** |
| FIX-12 | 产品 ninja（base/update） | `FAILED: updater/updater/yacc/lexer.cpp updater/updater/yacc/parser.cpp`，真因：`generate_yacc.py` 抛 `FileNotFoundError: [Errno 2] No such file or directory: '/usr/include/FlexLexer.h'` | **精简镜像没装 `libfl-dev`**：`FlexLexer.h` 在 Ubuntu 22.04 由 `libfl-dev`（不是 `flex`）提供。注意报错只显示"FAILED yacc/lexer.cpp"这种**生成产物名**，真因在 Python traceback 里 | `apt-get install -y libfl-dev flex`，装完可单测：`generate_yacc.py --output /tmp/yacc_test --bisoninput ... --flexinput ...`（应产出 lexer.cpp / parser.cpp / location.hh 等） |
| FIX-13 | hb 启动期（非 ninja） | 日志里**没有 FAILED、没有编译错误**，却在 hb 初始化阶段 `code=exited status=1`：`File "build/hb/resolver/build_args_resolver.py", line 365, in resolve_ccache → os.path.join(ccache_base, ccache_local_dir) → TypeError: expected str, bytes or os.PathLike object, not NoneType`；伴随症状是 hb 随后又报 `error.log: No such file or directory`（把真因盖掉了） | **`HOME` 环境变量缺失**。hb 的 `resolve_ccache()` 用 `os.environ.get('HOME')` 推导 ccache 目录，没有兜底；从**非登录 shell**（`bash script.sh`）或 **systemd 单元**拉起构建时 HOME 为空 → `os.path.join(None, '.ccache')` 直接抛异常。与内存、会话、ccache 设置**都无关**（曾误判为 OOM/换页，浪费一轮排查） | 启动器里显式导出：`export HOME=/root; export CCACHE_BASE=$HOME; export CCACHE_LOCAL_DIR=.ccache`（见 `/src/run_build.sh`）。**判据**：日志出现 `[launcher] HOME=...` 且不再有 `resolve_ccache` 字样。另建议用 `systemd-run --unit=ohbuild --collect /src/run_build.sh` 或 `tmux` 托管，彻底免于会话退出 |
| **FIX-14** | 构建并发（**决定成败**） | 构建慢到不可用（重型 C++ 阶段 **~4 目标/分钟**），内存常年打满、swap 涨到 6–15 GB、`Cached` 只剩 100–700 MB（页缓存被挤空 → 每次编译都重新从磁盘读头文件）。`--jobs 10/6/4/3` **全都一样**，怎么调都没用 | **OH 7.0 的 hb 里 `--jobs` 是空实现**：`build_args_resolver.resolve_jobs()` 直接 `return`（源码注释 `# PlaceHolder`），参数不会传给 ninja。所以 ninja 用**默认并发 = CPU 核数**（本机 16）→ 18 个 clang++ 同时跑 → 12 GB 内存瞬间爆掉。**真凶不是内存不够，是并发没被限制** | 用 hb 支持的透传参数：**`./build.sh --product-name x86_64_virt --ccache --ninja-args=-j6`**（`--ninja-args` 会把参数逐个追加到 ninja 命令行）。**验证**：`ps -ef \| grep '[n]inja -w dupbuild'` 应看到 `... images -j6`。实测效果：并发 clang 18→6、swap 6.7 GB→0.7 GB、速率 **4 → 135 目标/分钟（30×）** |
| **FIX-15** | 产品 ninja（third_party） | `FAILED: obj/third_party/libphonenumber/.../geocoding_data.o`，但编译器报的却是 **C++ 语法错误**：`geocoding_data.cc:1:1: error: unknown type name 'version'` / `unknown type name 'oid'` | **LFS 指针出现在"源码文件"里**：`geocoding_data.cc` 本身是 131 字节的 LFS 指针（`version https://git-lfs.github.com/spec/v1`），于是 clang 把这三行指针当 C++ 源码编译。**上一轮全树扫描漏了它**——因为按"压缩包/二进制后缀"过滤（`*.tgz/*.so/*.bin…`），而**指针也会出现在 `.cc/.h/.png/.ttf` 等源码与资源文件里**。用 ripgrep 按**内容**重扫，全树共 **1551 个**指针（涉及 18 个仓库，含 `third_party/icu`、`base/web/webview`、`base/global/font_manager`、`foundation/graphic/*`、`foundation/multimedia/*` 这些在产品里的仓库） | 按仓库 `git lfs pull --include="<文件列表>"`。**教训：判断 LFS 指针必须按内容扫（`rg -l 'git-lfs\.github\.com/spec'`），不能按后缀过滤。** 本次已把 8 个关键仓库清零；剩余 604 个都在 `test/xts`、`third_party/noto-cjk`(字体)、`third_party/vk-gl-cts`、`docs`，不影响编译 |

> 依赖提醒（按踩坑顺序）：**python**（FIX-7，`ln -sf /usr/bin/python3 /usr/local/bin/python` 或 `apt install python-is-python3`）、
> **autotools 等**（FIX-8 那行 apt 命令）、抓图用 `socat`；`default-jdk / libtinfo5 / genext2fs / mtools / u-boot-tools / mtd-utils` 是 `tools_checker.py` 会提示的可选项。
>
> 上游 hb 插件路径：ERR-2 的 bug 在 `build/hb/util/loader/subsystem_info.py` 的
> `merge_subsystem_overlay()`；本轮修复全部落在**配置层 + 系统依赖层**（未改 hb 源码，SDK 的 cortex-m 目标也未改源码），
> 便于用官方源码树复现。

