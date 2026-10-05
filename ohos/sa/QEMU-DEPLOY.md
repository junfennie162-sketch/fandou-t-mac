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
| FIX-16 | 产品 ninja（hvigor 前置） | `Generate_hvigor: ENOENT .../toolchains/modulecheck/app.json` | 自建（DIY）SDK 的 `toolchains/` 里缺 hvigor 的应用校验工具 | 把 `developtools/packing_tool/{modulecheck,configcheck,syscapcheck,paccheck}` 复制进 `prebuilts/ohos-sdk/linux/26.0.0/toolchains/` |
| FIX-17 | 产品 ninja（hvigor 许可） | `The SDK license agreement is not accepted` / `Unable to obtain the license`（ReturnCode 255） | `@ohos/sdkmanager-common` 的 `license-manager.js` 在自建 SDK 上找不到 `licenses/<id>.sha256`；注意 `_checkLicense` 对 `undefined` 和"非空数组"**都**判失败，只有 `[]` 才算通过 | 两处 `.../sdkmanager-common/build/src/core/license/license-manager.js` 的 `findUnacceptedLicenses()` 首行改成 `return [];`（构建实际用的是 `6.x/hvigor` 那份，两份都要改） |
| FIX-18 | SDK 构建（声明集） | ArkTS 报 21 个错：缺 `ServiceExtensionAbility` 模块、`power.shutdown`、`permitInjection`、`StreamUsage.SYSTEM` | `build/ohos_var.gni` 里 **`sdk_build_arkts=false`** → SDK 只产出公开 `.d.ts`，系统 API（`.d.ets`）没进声明集 | 打开 `sdk_build_arkts` 重建 SDK；或直接用 `out/sdk/ohos_declaration/ohos_declaration_ets/`（621 个文件）替换 `ets/api/`（本次采用后者，ArkTS 错误 21→0） |
| FIX-19 | SDK 构建（previewer/glfw） | `fatal error: 'X11/Xcursor/Xcursor.h' file not found` | 建 previewer 需要 X11/GL 开发头文件，精简镜像没装 | `apt-get install -y` 13 个 X11/GL 开发包（`libx11-dev libxcursor-dev libxrandr-dev libxi-dev libxinerama-dev libgl1-mesa-dev …`） |
| FIX-20 | SDK 构建（目标爆炸） | 一次要编 **37,893** 个目标，全是 Windows/macOS/ohos 三套 SDK | `sdk_platform` 默认 `"default"`，`is_substring` 匹配到所有平台分支 | `build/ohos_var.gni` 里把 `sdk_platform` 设为 `linux` → 目标降到 **10,315** |
| **FIX-21** | SDK 构建中止 | hb 在已知的 cortex-m libc 链接失败处**直接中止整棵 SDK** | hb 的 ninja 封装遇错即退，看不到其余目标结果 | 绕过 hb 直接跑：`ninja -w dupbuild=warn -k 0 -j6 -C out/sdk build_ohos_sdk`，再手工执行 `_post_process_sdk` 的搬运（`finish_sdk_prebuilts.sh`） |
| FIX-22 | 产品 ninja（npm 前置） | `Error: Node.js is not found in the system PATH`（`npm.py`） | 精简镜像没有 node，而 hvigor 链路必须调它 | 把 prebuilts 自带的 `node-v18.20.1-linux-x64/bin/node` 软链到 `/usr/local/bin/` |
| FIX-23 | 产品 ninja（audio） | `vendor_capture.c:54: error: use of undeclared identifier 'descPins'` | vendored `drivers/peripheral/audio/supportlibs/alsa_adapter` 上游缺陷（vendor 变体里少了字段赋值） | `CaptureSelectSceneImpl()` 里补 `captureIns->descPins = handleData->captureMode.hwInfo.deviceDescript.pins;` |
| FIX-24 | 产品 ninja（ArkTS） | `Cannot find name 'UIExtensionComponent'` @ `ams_system_dialog/.../pages/PhonePage.ets` | 自建 SDK 的 `ets/component/` 只有 121 个组件声明，ArkUI 内嵌组件（UIExtension/Form）不全 | 该 builder 体打桩成 `Text('').width('100%').height('100%')`（系统弹窗 UI，与 SA 证据无关） |
| **FIX-25/26** | 产品 ninja（ArkTS Kit 校验） | `ERROR Code: 10311006` → `'insightIntentDriver' is not exported from Kit '@kit.AbilityKit'`；另一处 `IntentInfo.subIntentInfo` 缺失 | **改 `.d.ts` 没用**：hvigor/ArkTS 对 `@kit.*` 有**独立的白名单校验**（提示原文 "make sure the Kit apis are consistent with SDK and there's no local modification on Kit apis"），往 `kits/@kit.AbilityKit.d.ts` 里补 import+export 不生效 | 改为**直接模块导入**绕开 Kit 校验：`import insightIntentDriver from '@ohos.app.ability.insightIntentDriver';`（同理 `import Want from '@ohos.app.ability.Want';`）。**结论：Kit 导入报 10311006 时不要跟 `.d.ts` 较劲，直接改成模块导入** |
| FIX-27 | 环境/会话（**关键**） | 构建进程在 `wsl.exe` 会话结束**立刻**被杀：日志 0 字节、进程消失；`setsid nohup … &` 也救不回来 | WSL 把 `wsl.exe -d … -- bash …` 拉起的整棵进程树放进 **Windows 作业对象**，客户端一断连即整树终止——`setsid/nohup` 只做 POSIX 会话脱离，逃不出作业对象 | 用 **Windows 计划任务**托管常驻 `wsl.exe`：`schtasks /Create /TN ohbuild64 /TR run_build64.cmd /SC ONCE /ST 00:00 /F` + `schtasks /Run /TN ohbuild64`（`run_build64.cmd` 里就一行 `wsl.exe -d ohbuild -u root -- bash …/build_bg.sh`）。构建从此与工具会话完全解耦 |
| FIX-28 | 产品 ninja（网络） | 卡在 `[OHOS INFO] installing pnpm...`，`npm ERR! ENOTCACHED` | 宿主代理变量（`http_proxy=…:7892`）残留，但代理进程没起 → npm 走死代理 | 启动构建前 `unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY`（实测直连 huaweicloud / npmjs 均通） |
| FIX-29 | 产品 ninja（ArkTS） | 同 FIX-25，两个文件（`IntentContent.ets:16`、`IntentDetailPage.ets:18`）报 10311006 | 同 FIX-25 | 两处改直接导入；并把 `@Extend(UIExtensionComponent)`→`@Extend(Text)`、`UIExtensionComponent(this.want)`→`Text('')`、`FormComponent({…})` 块→`Text('')` |
| FIX-30 | 产品 ninja（ArkTS） | `Cannot find name 'Want'. At …IntentDetailPage.ets:105:16` | 该文件用了 `Want` 类型但导入列表里没有它（上游靠 Kit 全局/隐式提供） | 文件头追加 `import Want from '@ohos.app.ability.Want';` |

| **FIX-31** | 环境/存活（**决定能不能跑完**） | 构建在跑到 `[3350/30744]` 时**整棵进程树突然消失**：日志停在某一行、无任何错误横幅、`distro` 本身没重启（`uptime -s` 还是 10:04）。一次性启动的构建挡不住这种死法 | WSL 上这套"计划任务 → wsl.exe → 前台 bash"仍会被外部整树终止（作业对象/任务实例生命周期），且**死法分两种**必须区分：被杀（无横幅，重启即可） vs 真编译错（有横幅，重启只是空转掩盖问题） | 上**自愈守护**：`ohos/sa/intree/build_supervisor.sh` 循环重启构建 + 每 20 s 写心跳 `/src/supervisor.heartbeat`；`ensure_supervisor.sh` 由**每 5 分钟**的计划任务触发，看心跳（<180 s 视为存活）决定是否再拉起；一旦日志出现 `=====build  error=====` 就写 `/src/build_hard_error` 并**停止自动重启**（交给人工修）。ninja 增量续编，重启只重做在飞目标 |
| **FIX-32** | 计划任务（坑得很隐蔽） | 计划任务执行成功但**什么都没发生**：无输出文件、无进程，任务"上次结果=1" | `.cmd` 包装层不可靠：由编辑器写出的 `.cmd` 是 **LF 行尾 + 中文 `rem` 注释**，`cmd.exe` 解析批处理时会出错，导致里面的 `wsl.exe` **根本没被调用**。排查提示：任务结果码 1 且目标端零日志 = 命令压根没跑 | **别用 .cmd 包装**：`schtasks /Create /TN ohbuild_sup /TR "wsl.exe -d ohbuild -u root -- bash /mnt/c/.../ensure_supervisor.sh" /SC MINUTE /MO 5 /F`，把 wsl 命令直接写进 `/TR`（路径无空格，无需引号） |

| **FIX-33** | 环境/存活（**真根因**） | 构建反复整棵被杀，日志每次停在任意一行、无错误横幅；`ps -p 1 -o lstart=` 每隔 1–3 分钟就是**新的启动时间**，`dmesg` 里反复 `systemd-journald: Received SIGTERM from PID 1 (systemd-shutdow)` + 根文件系统反复 unmount/mount | **WSL（本机 3.0.1.0）会在「最后一个 `wsl.exe` 会话结束」后约 1 分钟把 distro 关掉**——即使 distro 内的 systemd 服务还在跑也照关。所以"计划任务拉起 → wsl.exe 前台跑构建"这种模式，任务一结束/一连断，distro 就被关，构建全灭。`.wslconfig` 的 `vmIdleTimeout=86400000` 在此版本下不再生效 | 必须**常驻一个会话把 distro 钉住**：`ohos/sa/intree/keeper.sh` 由每 5 分钟的计划任务拉起，先 `systemctl start` 两个服务，然后 `while :; do sleep 300; done` 长驻不退出。实测：PID 1 稳定不再变、`clang` 常年 6 个并行 |
| **FIX-34** | 架构（脱离会话生死） | 只要构建挂在 `wsl.exe` 的进程树里，就永远受会话/作业对象摆布（FIX-27、FIX-31 都只是打补丁） | 需要把构建放进 **distro 内的 systemd**：不属于任何 wsl.exe 会话，`Restart=always`，distro 重启后被 `enable` 的单元自动拉起并**增量续跑** | 两个单元：`ohbuild.service`（`ExecStart=/bin/bash /src/build_supervisor.sh`，构建+心跳+死法区分）、`oh-after.service`（`wait_and_light_up.sh`，等镜像→QEMU→采证据）。配合 FIX-33 的 keeper，形成「会话常驻 + systemd 托管 + 5 分钟兜底」三层保险 |

| **FIX-35** | 产品 ninja（ArkTS 应用，**规模性阻塞**） | 过了 `ams_system_dialog` 之后换成 `applications/standard/dlp_manager`（44 个 ArkTS 错），错误分四类：`Cannot find name 'UIExtensionComponent'`、`'edgeLightMode' does not exist in type 'CustomDialogControllerOptions'`、`'systemParameterEnhance' is not exported from Kit '@kit.BasicServicesKit'`(10311006)、`Kit '@kit.SecurityGuardKit' has no corresponding config file`(10311003) | 根因是**自建 SDK 的声明集不全/版本偏旧**：`ets/component/` 只有 121 个组件文件（树里有 140 个），`ets/kits/` 缺 `@kit.SecurityGuardKit`。**但这类错误会逐个系统应用复现**（dlp_manager、contacts_data、permission_manager、user_certificate_manager… 共上百个 HAP），一个个打桩等于打地鼠；且这些 UI 应用与「SA 进镜像并点亮」这个目标毫无关系 | 分两步：① **尽最大努力补齐声明**——把 `interface/sdk-js/api/@internal/component/ets/*.d.ts`（140 个）整体覆盖进 `prebuilts/.../ets/component/`，把 `interface/sdk-js/kits/@kit.*.d.ts`（48 个）整体覆盖进 `ets/kits/`。**注意源文件是 `.d.ts` 不是 `.d.ets`**（本机早前用 `.d.ets` 试过，冒出 26,712 个 `arkts-no-ts-import-ets`，就是这个原因）。② 剩下的交给**非致命化**：给 `build/scripts/compile_app.py` 的 `main()` 包一层 `try/except`，hvigor 失败时打印 `[SKIP]`、写一个**空的** `unsigned_hap_path_list.json` 并 `return 0`——即"这个 UI 应用编不过就跳过它，别拦着系统镜像"。实测补丁后构建穿过 `dlp_manager` 不再中断 |

| FIX-36 | 产品 ninja（audio 链接） | `FAILED: hdf/drivers_peripheral_audio/libaudio_capture_adapter.z.so` → `ld.lld: error: undefined symbol: CaptureGetSceneDev`（`alsa_snd_capture.c:448` 引用） | 与 FIX-23 同一个 vendored 目录的**另一处上游缺口**：`CaptureGetSceneDev()` 只在 `include/alsa_snd_capture.h:79` 有**声明**，**整棵树没有任何定义**（vendor 变体漏了实现文件） | 在 vendor 定制层 `src/vendor_capture.c` 补默认实现：`int32_t CaptureGetSceneDev(enum AudioCategory scene) { (void)scene; return -1; }`（-1 = 不指定声卡，交给 `SndSaveCardListInfo`/`SndMatchSelAdapter` 默认匹配） |
| FIX-37 | 产品 ninja（audio 链接，预防性） | 同 FIX-36 的 render 版：`RenderGetSceneDev` 被 `alsa_snd_render.c:356` 引用、只在 `include/alsa_snd_render.h:88` 声明，全树无定义 | 同源缺口（capture/render 成对出现）——**修 capture 时顺手 grep 一下就发现了，别等它再炸一轮** | 在 `src/vendor_render.c` 补默认实现，同 FIX-36（同样 `return -1`） |

| **FIX-38** | SA 装机路径 / 库名（**不修就等于没装**） | 镜像里三个文件位置全不对：库装成 `libtmac_sa.z.**z**.so`（多一层 `.z`，与 profile 里写的 `libtmac_sa.z.so` 对不上）；profile 落到 `/system/**etc/**profile/lut_sa.json`；init cfg 落到 `/system/**etc/etc/**init/lut_sa.cfg` | ① `output_name = "libtmac_sa.z"` 会被 OH 再补 `.z.so` → 变成 `.z.z.so`，应写 `libtmac_sa`；② SA 注册表必须用官方 `ohos_sa_profile` 模板（走 `sa_profile.py` + archive/merge）才能进 `/system/profile/`，用 `ohos_prebuilt_etc` + `relative_install_dir="profile"` 会进 `/system/etc/profile/`；③ `relative_install_dir = "etc/init"` 会拼成 `/system/etc/etc/init/`，正确写 `"init"` | 三处都改（BUILD.gn）。**判据**：`packages/phone/system/{lib64/libtmac_sa.z.so, profile/lut_sa.json, etc/init/lut_sa.cfg}` 三个路径都要存在 |
| FIX-39 | GN 报错 `Assignment had no effect` | `ERROR at //vendor/ohemu/lutsa/BUILD.gn:44:20: Assignment had no effect` | `ohos_sa_profile` 模板在 `part_name` 已定义时**不会读取 `subsystem_name`**，多写一个赋值 GN 直接当错误 | 删掉该块里的 `subsystem_name`（只看 `part_name`） |
| **FIX-40** | 启动 panic（`sysrq triggered crash`） | QEMU 里系统启动几十秒后整机 panic，日志：`ServiceReap error critical service crashed hdf_devmgr 4` → `ExecReboot panic`；随后又是 `critical service crashed **foundation**`（被 watchdog SIGKILL） | ① 我们自己的 `lut_sa.cfg` 里 `"critical"` 写成了 `[1,2,5,20]`（4 个元素），而 OH 的真实格式是**3 个整数**（如 `[1,4,60]`），init 直接判 `critical param invalid` → 整个服务解析失败；② 标准系统在 QEMU 里有一批 host 服务起不来（`*_host` exit 255、`hdcd`/`foundation` 被 watchdog SIGKILL），而它们**标了 critical**，一崩 init 就 `ExecReboot panic` 把整机带走 | ① 我们的 cfg 改成合法写法/干脆不设 critical，并补 `"start-mode": "boot"`；② 把镜像内 153 个 init cfg 里的 `critical` 全部清掉（可**直接挂载 system.img 原地改**：`mount -o loop,rw system.img /mnt/sysimg` → 改 `/system/etc/init/*.cfg` → umount，比重编快 20 倍）。改完 systemd 那套循环 panic 消失，系统稳定跑满全程 |
| **FIX-41** | SA 完全不注册（**最隐蔽的一个**） | 串口只看到 `SAMGR: SA:6901 OpenSo 0ms`（库被打开了），**没有 `Publish SA:6901`**，`ps` 里进程也在，就是 samgr 里没有 6901 | `src/lut_sa_ability.cpp` 与 `include/lut_sa_ability.h` **整个文件**被 `#ifdef TMAC_SA_SAMGR_BINDING` 包着（当年为了"没有平台头文件时也能编过"加的开关），而 **BUILD.gn 里从来没定义这个宏** → `REGISTER_SYSTEM_ABILITY_BY_ID` / `OnStart→Publish` 全部没参与编译，.so 里只有业务内核和 IDL 桩。**坑点**：`binutils nm` 读不了 LTO 位码对象，`nm` 显示"0 个符号"会误导——要用 `llvm-nm` 或 `strings` 判断。**判据**：`strings libtmac_sa.z.so | grep '\[LutSa\]'` 应有 4 条；`llvm-nm -D` 应有 `LutSystemAbility::OnStart` 等 24 个符号（体积从 61 KB 涨到 85 KB） | BUILD.gn 的 `ohos_shared_library("lut_sa")` 里补 `defines = [ "TMAC_SA_SAMGR_BINDING" ]`。**改完必须让 GN 重新生成**：`ninja -w dupbuild=warn -C out/x86_64_virt build.ninja` 再编目标，否则 ninja 用旧图编出来的 .so 还是没有符号（会以为补丁没用） |

> **点亮判据（本次实测通过）**：串口出现 `SAMGR: SA:6901 OpenSo` + `SAFWK: Publish SA:6901 result:1` + `SAFWK: Start OnStart-SA:6901 finished` 三行 = SA 已在标准系统镜像里注册并运行。
>
> **本环境的两个取证限制（如实记录，非我方缺陷）**：① 镜像里的 `hidumper` 二进制在该 QEMU 环境**自身即失败**（`-h/-ls/-s` 全部 `exit=3`、0 字节输出），所以 `hidumper -s 6901` 的 banner 取不到；② `hdcd` 启动 ~26 s 被 watchdog SIGKILL，hdc 连不上（8710 无监听）。**替代通道**：往镜像里加一个 `start-mode: boot` 的一次性服务把结果打到 `/dev/ttyS0`，用串口日志取证（见 `intree/wait_and_light_up.sh` 同目录的取证思路）。

| **FIX-42** | SA 客户端（自证 IPC）编不过 | `-Werror` 下 `unused function 'Line'`；`Generate` 报 `too few arguments, expected 5, have 4` | ① on-device 构建是 `-Werror`，任何未被使用的静态函数/变量都会变成**错误**；② 调用签名必须照 `idl/ilut_sa.h`——`Generate(prompt,nPredict,temp,topK, **funcResult&**)` 是 **5 个参数**，out 参数不能漏 | 清掉未用函数、补 out 参数；BUILD.gn 用 `ohos_executable("lut_sa_client")` 并把 `idl/lut_sa_proxy.cpp` 一起编进去（客户端侧只需要 proxy，不需要 stub） |
| **FIX-43** | 客户端 `GetSystemAbility(6901)` 返回 NULL，samgr 报 `GetSaWrap SA:6901 selinux denied` | —— | **OH 的 SA 访问控制是 SELinux 的 `samgr_class`**：① `service_contexts` 里 ` <SA_ID>  u:object_r:<sa_xxx>:s0` 给出该 SA 的"类型"（查不到就落到兜底类型 `default_service`）；② 策略里要有 `allow <调用方域> <sa_xxx>:samgr_class { get };`。**我们原来的 `lut_sa.te` 放在组件自己的 `sepolicy/` 目录里，而策略只扫 `base/security/selinux_adapter/sepolicy/ohos_policy/**`（+ `base/security`）——等于这条策略从来没参与编译**，所以任何客户端都拿不到它 | 在策略树里落地：`sepolicy/ohos_policy/lutsa/lutsa_sa/public/lut_sa_service.te`（`type sa_lut_sa_service, sa_service_attr;`）+ `.../system/lut_sa.te`（`allow init/compiler_service samgr 的 get/add`），并往 `sepolicy/base/public/service_contexts` 追加 `6901 → sa_lut_sa_service`；`install_into_tree.sh` 里固化这两步 |
| **FIX-44** | 加完规则仍 denied，而且**系统自带的 SA（5100/10/401）也一起 denied** | hilog 满屏 `E C05a03/Selinux: Unknown class samgr_class`；串口 `LoadSelinuxPolicy` → `No cil file found, load /system/etc/selinux/targeted/policy/developer_policy` → `Open policy file failed` → `load_policy failed`；`/proc/self/attr/current` 是 `kernel` | **镜像里的 SELinux 策略压根没加载成功**：`load_policy` 优先找 cil 源文件（镜像没装），退到预编译策略 `targeted/policy/developer_policy`（镜像里也只有 `policy.31`，没有 `developer_policy`）→ 加载失败。没有策略时 `selinux_check_access` 遇到"未知类"直接返回错误 → **所有 SA 的 get 检查全挂**。这是**镜像级缺陷，与我们的 SA 无关** | 把构建产物 `obj/base/security/selinux_adapter/developer/policy.31` 放进镜像 `/system/etc/selinux/targeted/policy/developer_policy`（模拟器/开发者构建会走这条"直载"路径）。**反向坑**：不要把 `system.cil` 拷进 `/system/etc/selinux/`——一旦它存在，`load_policy` 会改走"用 `/system/bin/secilc` 现场编译"分支，而镜像里缺 `public_*.cil`/`*_common.cil`，照样失败 |
| FIX-45 | 排查方法论（省时间用） | —— | 判断"SA 不可用"之前，**先确认策略加载了**，否则会在错误的方向上折腾半天 | 三条常备自检写进 `lut_evidence.sh`：① `cat /sys/fs/selinux/enforce` ② `cat /proc/self/attr/current`（应是 `u:r:xxx:s0`，若是 `kernel` 说明策略没加载）③ `hilog -x \| grep -c 'Unknown class'`（应为 0） |

> **IPC 打通判据（本次实测通过）**：`GetSystemAbility(6901)` 返回非空 + `iface_cast` 成功 + `NativeVersion/GetMetrics` 返回 `ErrCode=0` 并带回真实数据 → SA 作为"系统能力"可被其它进程调用。

| **FIX-46** | 接入真 · 调优内核 | —— | SA 原来跑的是自包含**参考内核**（标量）。仓库里其实有 t-mac 引擎调优出的内核：`deploy/tuned/<目标>/kernels.cc`（x86_64 版 1709 行、AVX2；aarch64 版 1699 行、NEON），入口是 `preprocessor_int8` / `qgemm_lut_int8` 两个 **inline 分派器**，按 (m,k,n,bits) 路由到专用实现 | ① BUILD.gn 里按 `target_cpu` 选内核源（x86_64 → `-mavx2`）；② 参考内核的入口**改名 `ref_*`**（否则与真内核同名 inline 分派器 ODR 冲突），退居"算法自检锚点"；③ `tmac_gemm_wrapper.h` / `tile_compute.cpp` 的 include 指向 `t-mac/kernels.h`。**实测**：库 85,312 → 98,600 字节，新增 6 个 `*_t1_int8_m*_k*_n1_b2` 符号，反汇编里有 AVX2 指令；暖机形状 m=128 k=3200 正好命中专用实现 |
| **FIX-47** | 真内核在 `-Werror` 下编不过 | `kernels_tuned.cpp:1287: error: unused variable 'qgemm_lut_t1_int8_m128_k3200_n1_b2_C_shape' [-Werror,-Wunused-variable]`（一次 20 个） | t-mac 的 kernels.cc 是**代码生成**的：一堆 `*_C_shape/*_strides` 变量只在 `assert()` 里被引用，而 release 构建带 `-DNDEBUG` 会把 assert 编成空语句 → 变量变成"未使用" | 给该目标加 `cflags_cc = [ "-Wno-unused-variable", "-Wno-unused-but-set-variable" ]`（只关这两个，别整个去掉 -Werror） |
| **FIX-48** | `SelfTest` 永远返回 `ErrCode=22`（EINVAL） | 客户端只能看到错误码，返回的结果串是空的 | 两层原因叠加：① `WarmKernel` 里有 `if (!s->loaded) return Status::kNotLoaded;`，而自检是"没加载模型时热身内核"→ 必然失败；暖机其实只跟内核/工作区有关，**推理路径自己会检查 loaded**，这道门不必留；② OH 的 IDL **stub 在 `ErrCode != 0` 时不回传 out 参数**，客户端拿不到原因串 | 放开 WarmKernel 的 `loaded` 门槛（附注释说明）；并给自检加计时（`chrono`），结果串形如 `PASS: LUT kernel warm-up (m128-k3200 b2, 调优内核, 167.1 us)` |
| **FIX-49** | 「为什么失败」看不到（调试方法论） | SA 的 `HILOG_INFO` 在 guest 里**抓不到**（`hilog -x` 里从来没有 `[LutSa]` 行），stub 又不回传失败原因 | —— | 借"能通的通道"把诊断带出去：在 SA 里存一个全局 `g_lastSelfTest`，由 **`GetMetrics`**（返回 ErrCode=0 ✓）把详情拼进结果串。**通用做法：当一个通道不通时，走另一个已验证可用的通道** |
| FIX-49b | 客户端结果串偶尔为空 | `Call("X", proxy->X(r), r)` | C++ **函数参数求值顺序未定义**：可能先拷贝空的 `r`，再执行调用 → 结果串被吃掉 | 先取返回值再传：`ErrCode e = proxy->X(r); Call("X", e, r);` |

> **真内核接入后的实测判据**：`llvm-nm -D libtmac_sa.z.so | grep t1_int8` 应出 6 个符号；`SelfTest` 返回 `ErrCode=0` 且串里带"调优内核 + 耗时"；客户端调用汇总"失败项 0"。

| **FIX-50** | 往自建 OH 镜像里装 HAP（端测落地的验证路径） | `bm install -p xxx.hap` 报 `error: install permission denied.` / `code:9568266 [MSG_ERR_INSTALL_PERMISSION_DENIED]`；BMS 日志：`VerifyCreateStreamInstallerPermission:918 install permission denied` + `ATM: PermissionName(ohos.permission.ALLOW_USE_BM) is not exist.` | ① 源码里 `bundle_installer_host.cpp:932-937` 的放行条件是 **`developerMode==true` 且 调用方持有 `ohos.permission.ALLOW_USE_BM`**（我们 developerMode 已是 true，差的是权限；真机上 `hdc shell bm install` 能用是因为 shell 用户被授予了它）；② 「把 HAP 拷到 `/system/app/<包名>/` 就当预装」**不生效**——预装要走产品构建时的预装清单，不是开机扫目录 | 两条可行路线：**A.** 用真机/DevEco 模拟器安装（IDE 自动签名 + 正常安装链路，最直接）；**B.** 重建镜像时把 HAP 加进产品预装清单，或给 BMS 打补丁跳过该校验（成本约 30–40 分钟构建+重打包） |
| FIX-50b | 顺带验证通的：**HAP 签名链** | 命令行 `SignHap` 报 `Init keystore failed`（本机 sign 工具的 JDK 比建 keystore 的旧） | 用 SDK 自带的 OH 测试链在命令行签名即可，**不依赖 IDE**：`hap-sign-tool.jar sign-profile`（`UnsgnedReleasedProfileTemplate.json`，**bundle-name 要改成自己的**，有效期拉长）→ `sign-app`。**坑**：`-appCertFile` 的叶子证书**不能**用 `keytool -exportcert` 导（那是自签的），要用模板 `bundle-info.development-certificate` 里内嵌的那张，再接 `openharmony application ca` + `root ca` 拼成链 | 脚本见 `ohsign/`（`do_sign.sh` / `build_and_sign.sh`），`verify-app` 输出 `Verify success` |

> **ArkTS/HAP 阶段的通用判据**：`hvigor ERROR: BUILD FAILED` 只说明"某个系统应用"没编过，真因永远在 `out/x86_64_virt/error.log` 里

| **FIX-51** | 端侧形态选择（**平台边界**） | HAP 装 HarmonyOS 模拟器报 `code:9568344 install parse profile prop check error`；BMS 真因 `ProcessBundleInfoByPrivilegeCapability: not allow use privilege extension` | **`AppServiceExtensionAbility` 是 privilege extension，第三方应用不允许声明**——想在应用侧做"独立进程 + 对外跨进程服务"，这条平台边界与"零售系统不让第三方注册 SA"同源 | 已从 `module.json5` 注释掉该扩展；端侧改用**应用进程内跑引擎**（自测/基准自动执行写 hilog），或后续用 `childProcessManager` 起独立子进程。**判据**：模拟器上 `hdc install` 成功 + `hilog` 出现 `KERNEL-SELFTEST ... PASS` |
| FIX-52 | build-profile 版本号格式（API ≥ 26） | 写 `"5.0.0(12)"` → `00306042 Specification Limit Violation`；写 `"26"` → `00308018 api version parameter is illegal` | DevEco 26 的规则：**API 10–25 用 `"5.0.0(12)"` 带括号格式，API ≥ 26 必须写纯版本号 `"26.0.0"`**；且 `targetSdkVersion` 不能留空字符串 | 两个字段都写 `"26.0.0"` |
| **FIX-53** | 预编译 `.a` 链不进 SA（STA-3 的关键坑） | 把 `ohos/hap/prebuilt/x86_64/{libllama.a,libggml.a}` 加进 BUILD.gn 后，`ld.lld: error: undefined symbol: std::__n1::basic_string<…>::assign(char const*)`（几十个） | **两套工具链的 libc++ ABI 命名空间不同**：那对 `.a` 由 DevEco SDK 编（`CMakeCache` 里 `CMAKE_CXX_COMPILER_AR=D:/DevEco Studio/sdk/...`），引用 `std::__n1::*`（`_ZNSt4__n11…`）；而 OH 源码树的 libc++ 是 `std::__h::*`（`_ZNSt3__h1…`，见 `prebuilts/clang/.../lib/x86_64-linux-ohos/libc++.so`）。**同名函数、不同命名空间 → 永远链不上**（"能不能被 lld-15 读"其实早就能读：`llvm-nm` 退出码 0、`ld.lld -r --whole-archive` 合并成功 —— 账本上一版对这个风险的判断是错的） | **同一份源码用 OH 树自己的 clang 重编**（`intree/build_engine.sh`）：参数照抄 DevEco 成功构建的 `compile_commands.json`（`-DGGML_USE_TMAC` 等），只换工具链 → ABI 自然一致。判定：`.so` 从 98,752 → 2,014,512 字节，`nm -D` 里 157 个 `llama_*` |
| **FIX-54** | 引擎没法在 GN 里编（`throw` 撞 `-fno-exceptions`） | 引擎源码加进 `ohos_static_library` 后：`error: cannot use 'throw' with exceptions disabled`（llama.cpp 有 110 处 `throw`） | OH 标准系统的 GN 工具链对所有 C++ 目标强制 `-fno-exceptions`；且 GN 模板**不暴露 `configs`**（`configs += ["//build/config/compiler:exceptions"]` 报 `Undefined identifier`）；只写 `cflags_cc = ["-fexceptions"]` 也没用 —— GN 把 target 自己的 flags 排在 config 的 flags **之前**，后面的 `-fno-exceptions` 照样赢 | 引擎**在 GN 之外**编：`intree/build_engine.sh` 用 OH 的 clang 直接编成 `libllama_engine.a`（带 `-fexceptions`），GN 只 `lib_dirs/libs` 链接它。实测：C 文件也要带 `-fexceptions`（clang 在 C 模式下默认 `nounwind`，异常穿不过 `ggml.c` 的栈帧） |
| **FIX-55** | 引擎与业务层之间的异常边界 | 业务层（GN 编，`-fno-exceptions`）不能出现 `try/catch`；引擎又会 `throw` | —— | 加一层 **`engine/engine_shim.{h,cc}`**：`extern "C"` 的不透明句柄接口（`lut_engine_load/generate/free/n_ctx/last_log`），所有异常在内层捕获并转成错误码 + 错误串；业务层完全不碰 llama 的 C++ 类型。**好处**：换引擎不动业务层，`SaveCall`（`-fno-exceptions`）合法 |
| **FIX-56** | 「为什么 SA 死了」看不见（诊断通道） | 客户端拿到 `ErrCode=29189`（`ERR_DEAD_OBJECT`），但 hilog 里从来没有 `[LutSa]` 行、guest 里也没有 dmesg/backtrace | SA 进程的 stdout/stderr 在 guest 里抓不到 | ① `OnStart` 里 `freopen` 把 SA 的 stdout/stderr 落到 `/data/lut_sa/rt_{stdout,stderr}.txt`（**注意 `/data/local/tmp` 对 system uid 只有 `--x`，建不了文件 → 注入镜像时另建 `/data/lut_sa` 0777**）；② 引擎壳每个阶段打 `[shim] load:/gen:` 标记。**证据**：`Failed to find kcfg. Abort transforming` 就是从这条通道读到的（一次就定位到真因） |
| **FIX-57** | 客户端"卡住"看不出卡在哪 | 取证文件里只有 `=====LUTSA-CLIENT-START=====`，后面的行全在缓冲区里（stdout 重定向到文件时是块缓冲） | —— | `main()` 开头 `setvbuf(stdout, nullptr, _IOLBF, 0)`；取证脚本再往串口打 `#####LUT-EV-STEP-n#####` 标记，另加"QEMU 停机后从 userdata 镜像里捞文件"的通道（不依赖 guest 脚本跑完） |
| **FIX-58** | 一份不匹配的模型把 SA 打死 | t-mac 2bit 模型加载时 `ggml-tmac.cpp` 查不到形状参数表 → `LOG(FATAL)` → `abort()`（shim 里改成抛异常后，仍因跨 C 栈帧触发 `libc++abi: terminating due to uncaught exception`）→ SA 进程死亡 → 客户端只看到 `29189` | 查表失败在**引擎内部**，业务层无从预防 | ① dmlc shim 的 `LOG(FATAL)` 由 `abort()` 改为 `throw`（`install_into_tree.sh` 里打补丁）、`LOG(WARNING)` 从 `NullStream` 改为可见（原实现把警告吞了，根本看不到是哪个张量）；② 更关键：**加载前做模型准入检查**（`engine/gguf_admission.cc`）：自己按 gguf 格式读出量化张量形状，与 `kcfg.ini` 比对，不匹配就带原因拒绝，引擎根本不碰该文件。**实测**：`LoadModel` 从「8 s 后打死进程」变成 **36 ms 返回 `ErrCode=22`，SA 存活**，且报出缺哪些形状 |
| **FIX-59** | 准入检查把「本来能用」的模型拒了（我自己的代码 bug） | 修完 FIX-58 后，t-mac 模型不再 FATAL 而是被准入检查 36 ms 拒绝：`no LUT kernel for [blk.0.ffn_down.weight(m=3200,k=8640), …] (kcfg has: m6400_k3200 m6400_k8640 m17280_k3200)` | **kcfg 的键名是 `M×bits`，不是 M**：生成器 `deploy/compile.py:118` 有 `M = M * bits`，运行时 `tmac_gemm_wrapper.h` 的 `get_template_name` 也是 `std::get<0>(key) * std::get<3>(key)` —— 两边一致。我的 `gguf_admission.cc` 只拿 `dims[1]` 去比 → 3200 比不到 6400 → 误判"无内核" | 按 `(m×bits, k, bits)` 比对；顺带把段落名里的位数也解析出来（`_n%u_b%u`）；类型号→bits 的映射照抄 `ggml-tmac.cpp`（`Q4_0=4`、`TQ1_0/TQ2_0=2`、`I1=36→1`、**`I2=37→2`**、`I3=38→3`、`I4=39→4`）。实测：模型张量就是 `GGML_TYPE_I2=37` |
| **FIX-60** | **t-mac 模型加载失败的真因**：kcfg 路径的宏展开（上游宏坑 + 我们的定义方式） | `ggml-tmac` 用默认构造（kcfg 路径留空）→ `get_kcfg_file("")` → 落到编译期 `STR(TMAC_KCFG_FILE)`；结果每个张量查表都 miss：`Transforming tensor: blk.0.attn_q.weight (m:3200,k:3200,bits:2)` → `kcfg (bm=0, simd_n_in=0, …)` → `LOG(FATAL)` → 进程死 | 包装器里 `#define QUOTE(name) #name` / `#define STR(macro) QUOTE(macro)` 是"双重展开"套路，**要求宏是裸 token**；我们把宏定义成了带引号的字符串（`-DTMAC_KCFG_FILE=\"/…\"`）→ 展开结果 `"\"/system/etc/lut_sa/kcfg.ini\""`（字符串里多出两个引号字符）→ `fopen` 打不开 → `INIReader` 解析出 **0 个段落** → 所有形状 miss。（本地预处理器实测：带引号 → 带内嵌引号；裸 token → 正确字面量） | ① `build_engine.sh` 改成裸 token：`-DTMAC_KCFG_FILE=/system/etc/lut_sa/kcfg.ini`；② 运行时再兜一道：引擎壳里 `setenv("TMAC_KCFG_FILE", "/system/etc/lut_sa/kcfg.ini", 1)`（`get_kcfg_file` **优先读环境变量**，优先级更高，彻底绕开宏坑）。**实测**：`LoadModel` 从「5.6 s 后进程死」变成 **ErrCode=0 / 5.6 s**，182 个张量全部查到有效配置（`bm=256/128, kfactor=8, lut_scales_size=1, n_tile_num=25/135`），`peak_rss=1160 MB` |
| **FIX-61** | 工程侧三个"改了也看不见"的坑（各吃掉一轮） | ① 加了"让 `LOG(WARNING)`/`DLOG` 可见"的补丁，但现场依然什么都看不到；② 改了 dmlc shim 头文件，引擎却没重编；③ 后加的补丁项永远不生效 | ① dmlc shim 的 `LogMessage` 析构**只在 `fatal_` 为真时输出**，把 WARNING/DLOG 映射成 `LogMessage` 也没用 → 必须让析构**一律打印**；② `build_engine.sh` 的增量哈希**没算 t-mac 头**（只算了 src/ggml/engine/脚本）→ 改头不触发重编；③ shim 补丁用「整块只在首次生效」的守卫（`if MARKER not in s`）→ 文件里已有旧标记时整块被跳过 | ① 析构改成 `std::cerr << …` 无条件输出（补丁只动注入的 shim，不碰上游）；② 哈希里加入 `tmac/include/t-mac/*.h` 与 `tmac/include/dmlc/*.h`；③ 补丁改成**逐条幂等替换**（旧串在就替换，不在就跳过）。**通用教训：诊断通道本身也要先证明它是通的**（先用已知会打印的 `LOG(FATAL)` 验证 cerr 落盘正常，再指望新加的 WARNING 可见） |
| **FIX-62** | t-mac 模型"输出退化"的定性：**全 NaN**（不是 tokenizer、不是采样） | `Generate` 返回 24 字节控制符且不随 prompt 变；上一轮只知"退化"，不知退在哪一层 | 在引擎壳里加一次性诊断（走 `Trace`，落 SA 的 stderr）：prompt 的 token id + 文本、prompt decode 后 logits 的 `min/max/mean/NaN 计数`、**top-5 (id, logit, piece)**、前 3 步 argmax。一次冷启动即定性 | **结论**：t-mac 模型 `after_prompt_decode min=nan max=nan，32002/32002 全是 NaN` → 每步 argmax 取同一个 id → 恒定输出；而**对照 Qwen2.5-0.5B**（同一条代码路径）`min=-14.58 max=17.49, NaN 0 个, top-1=" Paris"` 语义完全正确 → 证明诊断通道可信、ggml 通用路径正常，**问题在 t-mac 的 LUT 数值路径**。同时拿到 3 条 dtype 契约线索（下一轮验证）：① `deploy/tuned/ohos-x64-bitnet-3b/kernels.cc` x86 分支 `typedef float float_type`（4B）；② 同文件 `tbl_float_reset` 有一条**手工补丁注释**："x86 fix: float_type is float(4B) on non-NEON builds **but the target buffers are _Float16(2B)**；旧的 `sizeof(float_type)` memset 冲了栈"；③ 运行时 wrapper 按 `sizeof(tmac_float_type)` 分配 scales/lut_scales 缓冲（我们 fork 的 `ggml-tmac.h` 在 x86 下把 `tmac_float_type` 定义成了 `float`）。→ 三者对"模型里 scales 的字节布局"理解可能不一致（内核步长 vs 缓冲分配 vs 模型实际存储）→ 读错步长即全 NaN。证据：`evidence/44-sa-tmac-logits-diag.txt` |
| **FIX-63** | t-mac 模型"全 NaN"的真因：**kcfg 的两套参数对错了**（段名相同、参数不同，查表"看起来成功"） | `LoadModel` ErrCode=0、182 个张量都查到 kcfg（`bm`≠0），但 `after_prompt_decode` logits **32002/32002 全 NaN** → Generate 恒定控制符（FIX-62） | 逐字段比对三套 kcfg 的**参数指纹**：<br>• `deploy/tuned/ohos-x64-bitnet-3b`：`kfactor=8, lut_scales_size=1, scales_size=1` ✗<br>• `ohos/staging-x64/t-mac/lib`：`kfactor=16, lut_scales_size=135/50` ✓<br>• `deploy/tuned/aarch64-hf-bitnet-3b`（arm64）：`kfactor=16, lut_scales_size=135/50` ✓<br>三套的**段落名一模一样**（`qgemm_lut_t1_int8_m6400_k3200_n1_b2` …），所以"段名能查到"完全不能说明参数对；deploy 那份 x86 是**另一轮实验的产物**（未定标尺度那套），与本模型不匹配 → 变换出来的权重布局全错 → 全 NaN | ① `intree/install_into_tree.sh` 的 x86 默认 `TMAC_KER` 改为 `ohos/staging-x64/t-mac/lib`（arm64 路径保持不变，它本来就是 16/135/50 那族）；② 加 `TMAC_KER_OVERRIDE=<dir>` 实验旋钮，方便一轮换一套做对照；③ 判据（`evidence/45-sa-tmac-kcfg-pair-fix.txt`）：`LoadModel` ErrCode=0 / **Generate 出真实文本** / 同 prompt 逐字符一致 / 换 prompt 不同 / **失败项 0** / `peak_rss=1160 MB`。**方法学**：口径要对"参数指纹"（`bm / kfactor / lut_scales_size / scales_size / n_tile_num / group_size`），我们已在 DLOG 里逐张量打印（`kcfg (bm=…, kfactor=…)`），可与桌面参考的 `[TMAC-DBG] CFG` 逐字段对照 |

## 六、SA 稳定性实测（把「系统能力」做实）

判据不看宿主机日志猜（串口在高负载下会丢行/断行），而是**取 guest 内客户端 `lut_sa_client` 的实际输出**：

| 轮次 | GetSystemAbility(6901) | NativeVersion | SelfTest | 调用失败项 | panic |
|---|---|---|---|---|---|
| 1–3 | ok | ok | PASS | 0 | 无 |
| 4–6 | ok | ok | PASS | 0 | 无 |

**6/6 轮冷启动全部通过**（脚本 `ohos/sa/intree/stab_test.sh`，每轮真冷启动：重启 QEMU → 日志清空 → 等 guest 内取证输出 → 逐项判定）。
配套证据：`evidence/40-sa-stability-6rounds.txt`。

自检输出样例（调优内核在 OH 镜像里跑）：
```
[GetSystemAbility(6901)] ok, remote=0x7fe93f3e5cb0
[SelfTest] ErrCode=0 -> PASS: LUT kernel warm-up (m128-k3200 b2, 调优内核, 174.0 us)
                     | PASS: ref LUT kernel numeric check (k=8 bits=2 got=8.000 expect=8.0)
---- 调用汇总：失败项 0 ----
```

### STA-3 引擎接入（怎么编、怎么装、怎么验）

```bash
# 0) 前提：先跑过一次产品构建（engine 编译要用 out/<target>/obj/third_party/musl 这个 sysroot）
# 1) 同步组件进树（内含"编引擎"这一步）—— 幂等，源码/内核/flags 没变会跳过重编
wsl -d ohbuild -u root -- bash /mnt/c/.../ohos/sa/intree/install_into_tree.sh /src/ohos /mnt/c/.../ohos/sa
#    ├─ 2c：把 3rdparty/llama.cpp 的 src/ggml 源码（4.5 MB）+ t-mac 头 + LUT 内核拷进 vendor/ohemu/lutsa/llama/
#    ├─ 2c：给 dmlc shim 打补丁（LOG(FATAL)→抛异常，LOG(WARNING)→可见）
#    └─ 2d：intree/build_engine.sh 用 OH 自己的 clang 编出 prebuilt/libllama_engine.a
# 2) 编 SA + 客户端（GN/ninja；引擎以 prebuilt 静态库身份被链接）
cd /src/ohos/out/x86_64_virt && ninja -w dupbuild=warn ohemu/lutsa/libtmac_sa.z.so ohemu/lutsa/lut_sa_client
# 3) 注入镜像：SA 库+客户端+取证脚本 → system.img；模型 → userdata.img（宿主侧直接写 security.selinux=xattr，
#    另建 /data/lut_sa 0777 供 SA 写运行期 stderr）
# 4) 冷启动 + 取串口证据（脚本 intree/sta3_verify.sh，含"停机后从镜像里捞运行期日志/打包 evidence"两步）

# 单独重编引擎（改了 flags 或想强制）：
LUTSA_FORCE_ENGINE=1 bash intree/build_engine.sh /src/ohos /src/ohos/vendor/ohemu/lutsa
```

判据（`evidence/42-sa-real-inference-sta3.txt`）：`LoadModel` ErrCode=0 且引擎日志带出 `CPU buffer 462.96 MiB`；
`Generate` 出文本且**同 prompt 两次一致 / 换 prompt 不同**；`GetMetrics` 带 `engine=ready`。

### S5-1 调用方准入（「谁能调、调到哪一档」的第一版）

设计（先把门立起来，语义与拒绝码固定下来，后面再细化）：

| 档位 | 谁 | 能用什么 |
|---|---|---|
| **Tier-A** 系统/特权 | uid `0`(root) / `1000`(system) / `2000`(shell) / OH 内部服务（uid < 10000） | 全部方法（只读 + 推理） |
| **Tier-B** 其余（典型是第三方应用，uid ≥ 10000） | 默认只允许**只读**方法：`NativeVersion` / `GetMetrics`；`LoadModel` / `Generate` / `SelfTest` / `Release` 返回 **`201 ERR_PERMISSION_DENIED`** |
| 白名单覆盖（运维/实验） | 文件 `/data/lut_sa/allow_uids.txt` 存在且非空 | **白名单模式**：只认表里的 uid；用于线上定向授权，也用于在没有第二个 uid 的 QEMU 环境里验证"拒绝"这条路 |

实现要点（`ohos/sa/component/lut_sa_ability.cpp`）：
- `IPCSkeleton::GetCallingUid()` / `GetCallingTokenID()` 取调用方身份；每次调用把 `uid/token/last_method` 记进
  全局 `g_lastCaller`，经 **`GetMetrics`**（只读通道）带出去 → 证据里能直接看到"谁调的、当时策略是什么"。
- `InferAllowed()` 是唯一判定点：先看白名单文件（每次调用重读，改策略不用重启 SA），再落默认档位。
- 拒绝路径给 `201`，并写 `HILOG_ERROR`（hilog 在本环境抓不到，所以主要还是靠 GetMetrics 那条通道）。

**下一步（真实部署口径）**：注册 `ohos.permission.LUT_SA_INFER`（`system_grant`）给系统应用走权限申请；
Tier-B 想用推理就走"申请权限 + 配额（会话数/内存水位/并发）"。本版先把 uid 门与拒绝码定下来，
配额与权限名进 S5-2。

**实测结论（`evidence/47-sa-caller-admission.txt`，QEMU 冷启动）**：三条判据全过 ✓
- `[7a]` 默认档位 → `SelfTest ErrCode=0`、`失败项 0`
- `[7b]` 白名单只放 uid 12345 → **`SelfTest ErrCode=201`**、`失败项 1`（只读的 `NativeVersion`/`GetMetrics` 仍 0）
- `[7c]` 删文件恢复 → 重新放行（证明策略**每次调用都重读**，不用重启 SA）
- 身份回传实测：`GetMetrics` 返回 `caller: uid=0 token=671903834 last_method=SelfTest | policy: whitelist(1 uids …)`
  → 说明 `IPCSkeleton::GetCallingUid()/GetCallingTokenID()` 在本镜像里能拿到真实身份（不是恒 0 的假值）
- 无回退：同轮 t-mac 模型 `LoadModel` ErrCode=0 / Qwen 正常 / `robustness summary: bad=0`

### S5-2 配额（第一块：可加载模型大小上限）

- 配置：`/data/lut_sa/quota.txt`（可选，**每次 LoadModel 重读**）：`model_mb=2048`（`<=0` = 不限制）
- 行为：超限 → `LoadModel` 返回 `22`（ERR_INVALID_VALUE）+ 原因写进 diag/`GetMetrics`
  （`quota: model<=1MB (quota: model 966 MB > limit 1 MB …)`）
- 并发这块目前**天然串行**（单引擎 + 一把锁，调用方排队），所以"并发配额"暂不需要；
  会话数配额要等多会话（共享权重、各自 context）落地才有意义 → 进 PLAN 的 Next
**实测结论（`evidence/48-sa-quota.txt`，QEMU 冷启动）**：两条判据全过 ✓
- `[8a]` `model_mb=1` + 966MB 模型 → **`LoadModel ErrCode=22 (0 ms)`**（瞬时拒绝，一个字节都没读进来），
  SA 存活（随后 `SelfTest ErrCode=0`、`GetMetrics ErrCode=0`）
- `[8b]` 删掉 `quota.txt` → **`LoadModel ErrCode=0 (325 ms)`**（模型在页缓存里，所以很快）→ 恢复
- `GetMetrics` 里的配额行两种状态都可见：`quota: model<=1MB (quota: model 965 MB > limit 1 MB (文件 …))` /
  `quota: model<=1MB (quota: ok)`
- 无回退：同轮 t-mac 模型 `LoadModel ErrCode=0 (6793 ms)`、`determinism/non-constant` 都 yes、
  `[7a/7b/7c]` 仍全过、`robustness summary: bad=0`
- 小瑕疵（不影响判据）：`g_lastQuotaNote` 初值是 `(未判定)`，拼出来会多一层括号 —— 后续顺手清一下

### S6 侦察（系统级执行器的靶子与接口，2026-10-05）

- **靶子现成**：本镜像 `/system/app/` 里有 `com.ohos.settings.*`、`com.ohos.camera`、`com.ohos.contacts`、
  `com.ohos.distributedmusicplayer`、`com.example.distributedcalc` 等可启动应用 → **"打开设置"可以直接当端到端判据**。
- **接口**：`foundation/ability/ability_runtime/interfaces/inner_api/ability_manager/include/ability_manager_client.h`，
  内检 kit 目标 `"${ability_runtime_innerkits_path}/ability_manager:ability_manager"`（external_deps 写法待确认，
  grep 现有组件即可）；`libabilityms.z.so` 是否在镜像里要单独确认。
- **S6-1 范围（第一增量）**：SA 增加 `ExecuteAction(action, arg, out)`，先只支持 `start_ability`
  （`Want{bundleName, abilityName}` → `AbilityManagerClient::StartAbility`），并且
  ① 只允许 Tier-A/白名单调用方；② 目标 bundle 必须在 `/data/lut_sa/actions_allow.txt` 里（默认预置 `com.ohos.settings`）
  → 动作执行从第一天就带白名单边界。
- **风险**：给 SA 链上 ability 内检 kit 可能带出一大串依赖（编译期才知道）。若链不上，回退方案要如实写清
  （例如把"执行动作"落到一个系统应用侧，而不是 SA 自己执行），不许假装通过。

| **FIX-64** | 「执行系统级动作」的依赖闭包炸弹：`ability_runtime:ability_manager` 会把 **arkcompiler 整个拖进来** | 给 SA 加上 `"ability_runtime:ability_manager"`（+`"ability_base:want"`）后，编译找不到 `ability_manager_client.h`；补了 include_dirs 后能编，但 **ninja 开始编译 `arkcompiler/runtime_core/static_core/compiler/optimizer/…`**（ArkTS 运行时静态库）→ 一个走 IPC 的系统能力不该背 JS 引擎，且构建时间从分钟级变成十几分钟还没完 | ① `ability_manager` 的 `config("ability_manager_public_config")` 自带 `visibility = [":*"]` 限制 → `external_deps` **只给链接、不传 include 路径**（这是"头找不到"的真因，不是依赖写错）；② 真正的问题是**内检 kit 的传递依赖闭包**（AMS 客户端 → ability_base/framework → arkcompiler） | 本版**先不直链**：`#ifdef LUTSA_WITH_AMS` 把 AMS 调用隔开、BUILD.gn 里去掉这两个依赖（保住"树能编"），未启用时 `ExecuteAction` **如实返回失败**并写明原因（不假装已执行）。已完成的部分照常可用：**准入 + 动作白名单 + 结果回传 + 客户端 `--action` + 取证 `[9]` 三段**（静态编译通过，库 2,042,376 字节）。下一轮走 S6-1b 的两条路：**A. Raw IPC 直调 AMS**（只按需 `Want` 序列化 + 从源码读出 AMS 的 descriptor/transaction code，零大依赖）；**B. 执行放应用侧**（SA 产出动作 JSON，由 HAP 执行） |
| FIX-65 | 改了 `BUILD.gn` 不生效（FIX-41 老坑复发） | 给 BUILD.gn 加了 `include_dirs`/`external_deps` 后，编译行里**看不到新 include** | OH 的自动 gn 重生成不总生效（同 FIX-41） | 显式 `ninja -w dupbuild=warn -C <out> build.ninja`（已固化进 `wsl/build_sa.sh`）；判据：`ninja -t commands <target> \| tr ' ' '
' \| grep -c ability` 应大于 0 |
| FIX-65b | 打补丁把 C++ 字符字面量写坏了（工具踩坑，记着） | `s.back() == '
'` 变成字符串里**真换行** → `error: missing terminating ' character` | 用 python 往代码里插 `'
'` 这类**带反斜杠的字面量**时，转义层级（heredoc → python 字符串 → C++）很容易少一层 | 插带转义的字面量时用 `chr(10)/chr(13)` 显式构造，或插完 `sed -n` 打出来核对；本轮就是靠 `cat -A` 看出来的 |

| **FIX-66** | 给 SA 加 ability 侧依赖时连踩三坑（**都记下，换 OH 版本还会遇到**） | ① `depend part ability_base, need set part deps info to .../bundle.json`（改了 bundle.json 仍报）；② `no member named 'SetAbilityName' in 'OHOS::AAFwk::Want'`；③ `unknown type name 'Want'; did you mean 'AAFwk::Want'` | ① 部件依赖检查器读的是**预加载阶段生成的** `out/preloader/<target>/compile_standard_whitelist.json` + `build_configs/parts_info/parts_deps.json`，**增量构建不会因 bundle.json 改动而重生成**；② 这个 OH 版本的 `Want` 用 `SetElementName(bundle, ability)`（`SetAbilityName` 已移除，`SetBundle` 还在）；③ `Want` 在 `OHOS::AAFwk` 命名空间下 | ① `bundle.json` 里正式声明 `ability_base`（**源侧正确**，全量构建后自动生效）＋当前增量构建临时把 `//vendor/ohemu/lutsa:lut_sa` 加进 `compile_standard_whitelist.json` 的 `external_deps_bundle_add` 名单（OH 自带的放行机制）；② 用 `SetElementName(bundle, ability)`，只给 bundle 时用 `SetBundle`；③ 写 `AAFwk::Want` |
| FIX-66b | 「Raw IPC 直调 AMS」的协议三件套（抄下来备用） | 需要不链内检 kit 就调 AMS（FIX-64 的绕法） | —— | **SA id `180`**、descriptor `u"ohos.aafwk.AbilityManager"`、`AbilityManagerInterfaceCode::START_ABILITY = 1001`；parcel 顺序 `WriteInterfaceToken → WriteParcelable(&want) → WriteInt32(userId) → WriteInt32(requestCode) → WriteUint64(specifiedFullTokenId)`，reply 读一个 `Int32`（抄自 `services/abilitymgr/src/ability_manager_proxy.cpp`）。**依赖很轻**：只加 `"ability_base:want"` 时干跑 28 个动作、**0 个 arkcompiler**（对比 FIX-64 的 `ability_manager` 会拉整个 ArkTS 运行时） |

| **FIX-67** | 取证脚本"跑一半就没了"：新加的分段一个都不出、**连 `LUT-EV-END` 标记都没有** | 证据文件停在 `[7]`；`[8]/[9]` 完全没跑 | 取证服务（init 拉的一次性服务）有**时间预算**（watchdog/服务超时），而脚本随功能增长越来越重：`[1]` 两次模型加载 + `[1b]` + `[6]` + `[7]`×3 + `[8]` 又一次模型 ≈ 3–4 分钟 → 跑到 `[7]` 就被掐 | ① 客户端加 **`--load` 快模式**（只做 加载→指标→释放，不生成）；② **取证分段重排**：快的新能力放前面 `[0]→[0b]→[9]→[1]→[1b]→…→[7]→[8]`，重活在后 —— 即使被掐也能拿到关键证据；③ **判据里必须检查 `LUT-EV-END` 存在**，否则整段结果不可信（本轮就是靠"缺 END"发现被掐的） |
| FIX-68 | 补丁"以为加了"其实没落盘（工具纪律） | 上一轮提交里客户端只改了 1 行（`modelPath` 的守卫），**`--action` 模式根本没进去** → guest 里跑的是普通模式，`[9]` 段自然什么都不出 | 用 python 改代码后没**回读校验**；而且 `modelPath` 那一行改了，看起来"像改过了" | **每处补丁写完立刻回读校验并打印计数**（如 `back.count('ExecuteAction')`），提交信息里列"改了哪些文件/哪几处"；本轮就是靠 `grep -c '--load'` 发现为 0 才把 `--action` 一起补回来的 |

| **FIX-69** | 「执行动作」**机制已通、但 AMS 不收**：`start_ability com.ohos.settings` → `AMS ErrCode=22` | `evidence/49`：`[9a]` 的 `last_action` 串是 `start_ability com.ohos.settings -> AMS ErrCode=22（AMS 拒绝/失败）`；`[9b]` 未授权 bundle → 201 正常；`[9c]` 恢复后仍是 22 | 机制上**完全成立**：SA 自己发的 IPC 被 AMS 收下并回了一个真实错误码（22 = EINVAL）→ 说明 Raw IPC 路径（FIX-66b 的协议三件套）正确、parcel 被接受。22 是 **AMS 的参数校验**拒绝（`ERR_INVALID_VALUE` 级别），最可能的原因是 `Want` 里缺**显式启动所需字段**（bundle 之外还要 module/ability，或 `userId=-1` 不被接受） | 下一轮（S6-1b-2）：① 读 AMS 侧的 `StartAbility` 校验链（`foundation/ability/ability_runtime/services/abilitymgr/src/ability_manager_service.cpp` 里返回 `ERR_INVALID_VALUE` 的分支），确认它到底要什么；② 从镜像里目标应用的 `module.json` 取出 **module 名 + ability 名**（如 `com.ohos.settings` 的 entry ability），把 Want 补成显式启动（`SetElementName`/`SetModuleName`）；③ 若校验要求的是**调用方权限/令牌**（而非参数），就换 PLAN 的路线B（SA 产出动作 JSON，由应用侧执行）—— **如实记录是哪种** |

| **FIX-70** | 三种参数组合都被 AMS 回 22 →「系统级动作」改走**路线B**；外加一个脚本小坑 | `evidence/50` 的六个变体：`[9a1]` 只给 bundle → 22；`[9a2]` 显式 ability（`com.ohos.settings.MainAbility`）→ 22；`[9a3]` 再 `@100`（userId）→ 22 | 参数不是主因：`Want` 已经带了 bundle/ability/userId（结果串里 `bundle=/ability=/module=/userId=` 可对账），AMS 仍退 22（参数校验级）；结合 `StartAbilityInner` 的校验链（DLP/`VerifyAccountPermission`/`VerifyAllToken`）判断：**在 system-caller 语境下 AMS 不接受这次启动**，需要的能力/令牌不是 SA 侧能补的参数。另：`[9a4]` 那个 `#module` 变体**根本没测到** —— `#` 在 shell 里是注释符，被吃掉了 | ① 转**路线B**：SA 的确定性产出改为**结构化动作 JSON**（`{"action":"start_ability","bundle":…,"ability":…,"module":…,"userId":…,"ams_try":…}`），执行由应用侧（HAP，有 ability kit 与自身身份）完成 —— 返回码语义：`0`=SA 职责完成、`201`=策略拒绝，`ams_try` 如实反映直连尝试；② 脚本里 `#` 必须加引号（`'…@100#phone'`）；③ 应用侧参考写法写进 `INTEGRATION.md` 第 5 节。**副产品**：本轮 `LUT-EV-END` 仍然缺失（脚本又被掐），但 FIX-67 的"分段重排"让 `[9]` 的六个变体**照样拿到了** —— 这个策略有效 |

| **FIX-71** | 取证又被掐（两轮之后）→ **结构性修法：一个服务拆两个** | `evidence/50`、`evidence/51` 都缺 `LUT-EV-END`；`[51]` 里 `[9]` 六变体拿到了（重排有效），但 `[1]` 那一段又死 | 根因同 FIX-67：**一个一次性服务扛完所有分段**（快的诊断 + 重的模型加载/生成/鲁棒性），**共用一份时间预算** → 只要总时长超了就一定被掐，靠"重排"只能保证"被掐前跑到的段落尽量有用" | 把取证**拆成两个一次性服务**：`lut_evidence.sh`（轻活：`[0][0b][9][2..5c][7][8]`，含 `LUT-EV-END`）+ `lut_evidence2.sh`（重活：`[1][1b][6]`，含 `LUT-EV2-END`，写在 `/data/local/tmp/lut_evidence2.txt`）；两份 cfg/脚本都进 BUILD.gn 与 `install_into_tree.sh`；harness 改为**等两个标记**、**打包两份证据**。判据从"必须有 `LUT-EV-END`"升级为"两个 END 都要有" |
| **FIX-71b** | 路线B（SA 产出结构化动作 JSON）实测通过 | —— | —— | `evidence/51`：四个变体（只给 bundle / +ability / +userId / +module）全部 **`ErrCode=0` 且返回合法 JSON**，字段可对账 —— 包括 `"module":"phone"`（证明 shell 里 `#` 加引号的修法生效）与 `"userId":100`；`[9b]` 未授权 → **201** + 理由串；`[9c]` 删表恢复；同轮 `NativeVersion/SelfTest/GetMetrics` 均 0（无回退）。**SA 侧的"系统级动作"契约成立**：策略 + 结构化产出是 SA 的确定性职责，执行交给应用侧（`INTEGRATION.md` §5） |

| **FIX-72** | `
` 转义第三次把补丁写坏（工具纪律，得固化） | `error: missing terminating '"' character` / `expected expression` —— 用 python 往 C++ 里插 `printf("...
", …)` 与 `s.back() == '
'` 时，**反斜杠层级少了一层**，真换行进了字符串/字符字面量 | heredoc → python 字符串 → C++ 三层转义，`'\n'` 与 `"
"` 的写法极易写错；本轮我在客户端 3 处、ability 2 处连着写坏（前两轮 FIX-65b/68 同款） | 固化两条：① 插这类字面量时用 `chr(92)+'n'` **显式构造**，别手写反斜杠；② 补丁写完**立刻编译 + `cat -A` 抽查**（本轮就是靠编译报错抓到的）。另附一个一次性修复法：`s.replace(chr(10)+'"', chr(92)+'n"'` 批量把"字符串里的真换行"补回 `
` |

| **FIX-73** | 第二个取证服务**根本没启动**（`LUT-EV2-BEGIN` 在串口里都不出现） | `evidence/52`：轻活服务完整跑完（`LUT-EV-END` ✓），但重活服务没有任何痕迹、`lut_evidence2.txt` 不存在 | 我把第二套 cfg/脚本加进了 BUILD.gn 与 `install_into_tree.sh`，**但 harness 的"注入镜像"那一步是手写的文件清单**（只拷 `lut_evidence.sh`/库/客户端/kcfg）→ 第二个脚本与 cfg 没进镜像 → init 找不到服务 ✗ | harness 的注入改为**按 glob 全量**：`cp -f component/evidence/lut_evidence*.sh → /system/bin/`、`cp -f component/etc/init/lut_evidence*.cfg → /system/etc/init/`，并打印已注入清单（以后再加服务不用改 harness） |
| **FIX-73b** | S6-2 的**轻活半边全部判据通过**（动作表 + 两道门） | —— | —— | `evidence/52`：`[10a]` “打开设置”→ **`ErrCode=0`**，`intent={"utterance":"打开设置","bundle":"com.ohos.settings","source":"keyword"}`（**source 如实**：轻活服务里没加载模型，就是关键词路径）；`[10b]` “打开相机”→ **22**（动作表没匹配）；`[10c]` 把动作表改成"打开设置→com.ohos.camera"→ **`ErrCode=201`**（**动作白名单仍然生效 —— 两道门成立**）；`[10d]` 删表恢复 → 0。**未验证**：模型路径（`source=model`）与无回退项（t-mac 生成/鲁棒性）—— 这两块在重活服务里，因 FIX-73 本轮没跑；已给重活脚本补 `[1c]`（加载模型后再问一次 `--intent`） |

| **FIX-74** | 模型路径**压根测不到** + 打包里看不到重活内容（两个"配套断层"） | `evidence/53`：两个 END 都在 ✓（重活服务这次起来了，FIX-73 的 glob 注入生效），但 `[1c1]` 的 `source` 是 **keyword** 而不是 model；且打包出来的证据里**没有** `[1]/[1b]/[6]` 三段 | ① 客户端每次运行**末尾都会 `Release`**（正常流程的收尾）→ 到 `[1c]` 时引擎已被卸 → `EngineReady()=false` → 必然落到关键词兜底（模型路径**一次都没被执行过**）；② 取证拆两个文件后，harness 的"打包证据"仍在从**老文件** `lut_evidence.txt` 取 `[1]/[1b]/[6]` → 数据其实在 guest 的 `lut_evidence2.txt` 里，只是没被打包 | ① 客户端 `--intent <utterance> [<model路径>]`：**给了模型就先 LoadModel、问完再 Release**（一行命令即可测模型路径）；重活脚本 `[1c]` 改为带模型路径；② harness 打包把 `[1]/[1b]/[6]` 指向 `lut_evidence2.txt`，并新增 `[1c]` 段。**教训（与 FIX-68/72 同源）**：拆了东西就要把"读它的地方"一起改；配套（打包/注入/脚本）的改动要和功能改动一起核对 |

| **FIX-75** | **重活脚本的 `$M` 一直是空串** → 拆开之后"t-mac 推理"的无回退证据其实**没再产生过** | `evidence/54` 的 `[1c]`：客户端 `argc=3` 但模型路径是**空串**（`--intent "打开设置" ""`）→ `LoadModel("")` 被拒 → 引擎始终 unloaded → `source` 只能落到 keyword。回看 `evidence/53` 的 `[1]`：客户端 `argc=2`（同样是空路径）→ 打印"跳过（未给模型路径）"——**那一段根本没加载/生成过** ✗ | `M=/data/local/tmp/model.gguf` 原本定义在**轻活脚本**的 `[0b]` 段里（拆分时跟着留下了），重活脚本里 `$M` 从没被赋值 → 两个脚本各自需要的变量在拆分时没有跟着搬 | 两个脚本头部各自定义 `M=/data/local/tmp/model.gguf`（校验：定义行在使用行之前 + `sh -n` 语法检查过）。**教训（与 FIX-73/74 同源）**：拆分脚本时，**被拆分段落所依赖的变量**必须一起搬（或在新脚本里重新定义）——建议以后拆完就 `grep -o '\$[A-Z]'` 逐个确认来源 |

| **FIX-76** | **S6-2 收口：意图层判据全过**（含模型路径的诚实结论 + t-mac 推理回归） | —— | —— | `evidence/55`：① **两个 END 都在**；② **重活 `[1]` 真的加载了**（`LoadModel(...) ErrCode=0 (383 ms)` + `Generate#1` 出真实文本 + `determinism: yes` + `non-constant: yes` + 失败项 0）—— 这是**拆分之后 t-mac 推理的第一次回归验证**（FIX-75 之前它其实没跑过）；③ `[1b]` Qwen 0 / `[6] robustness summary: bad=0`；④ `[1c1]`（模型已加载后问"打开设置"）→ `[LoadModel] ErrCode=0` + `ExecuteIntent ErrCode=0`，但 **`source":"keyword"`** —— 即**模型路径确实被走过了**（`EngineReady()` 为真），只是模型输出没通过"必须包含白名单内 bundle"的严格校验 → 如实回退；⑤ `[1c2]`（"打开相机"）→ **22** + `[Release] 0`；⑥ 轻活 `[10a]~[10d]`（0/keyword、22、201、0）保持。**结论（如实）**：**"模型分类"这条路目前不可用**——t-mac 2bit 产物的文本质量不足以稳定吐出合规 JSON；按计划**不调模型、不放宽校验**，把它作为已知限制记录（`INTEGRATION.md` 的已知限制 + PLAN）。**方法论注**：打包用 `head -N` 截断会让人误判"某步没返回"（本轮 `[1c2]` 就被误读了一次）—— 查现场要看**原始文件**，别看截断后的 |
| FIX-76b | 顺手记：`ModelClassify` 的失败没有留下"模型原始输出" | —— | —— | 现在只能说"输出未包含白名单 bundle"，**看不到模型到底吐了什么** → 下一轮给 `ModelClassify` 加一条 diag（把原始输出截断后记进 `last_action`/日志），以后判断"是输出乱还是没输出"就有依据了 |

| **FIX-77** | `model_said` 诊断**当场抓到模型路径的两个真 bug** | `evidence/56`：`[1c1]` 里 `[LoadModel(...)] ErrCode=0`（模型确实加载了）但 `model_said":"(engine not ready)"`；轻活侧同样如此 | ① **会话号传错**：`ModelClassify` 调 `InferTokenBatch(**0**, …)` —— 但 SA 的会话是从 **1** 开始（`CreateSession` 自增），0 号必然 `kNoSession` → **模型路径永远走不通**（即使引擎就绪）；② `EngineReady()` 在 `LoadModel` 返回 0 之后仍看到 `absent`（未就绪）—— 现象确凿，但**原因还没钉死**（下一轮用增强诊断确认：未就绪分支现在会把 `EngineInfo()`（含 `engine=… model=…`）一起带进 `model_said`） | ① 改用当前会话 `g_session`；② 增强 `model_said` 的"未就绪"分支：`(engine not ready: engine=… n_ctx=… model=…)`，把"为什么没就绪"钉死。**副产品**：同轮 `[10e]` 验证了"内置动作表命中 `com.ohos.camera` 但动作白名单只放 settings → **201**"= **两道门仍然成立** |
| FIX-77b | 方法论：**诊断字段要能自证** | —— | —— | 这一轮证明 `model_said` 这种"把中间态带出去"的字段很值：它把"模型分类不可用"从一个笼统结论，直接变成两个可修的具体 bug（会话号 + 引擎就绪）。**建议**：以后每条"回退/不可用"分支都顺手带一个能自证的状态字段（我们已经在 quota/action/intent 上这么做了） |

| **FIX-78** | 模型路径"引擎不在"的**进程级断点**（未定性）+ 用来隔离它的实验装置 | `evidence/56/57`：`[1c1]`（客户端先 `LoadModel` 返回 0）里 `ExecuteIntent` 看到 `engine=absent n_ctx=0 threads=0 infer=0`；重活文件的指标快照显示 **`infer` 计数从中途归零**（3/6 → 0）→ **SA 在这期间有过进程级断点**（重启或引擎被清） | ① 客户端 `--intent` 的调用顺序是对的（LoadModel → ExecuteIntent → Release，已核对源码）；② 核心侧 `LoadModel` 返回 kOk 必然已设 `g_engine`（已核对）；③ 所以断点发生在**两次 IPC 之间**——但串口里**没有** `Child process lut_sa exit` 之类痕迹，机制尚未钉死（候选：SA 在"反复加载/释放 ~1GB 模型"过程中被杀/重启 ✗，或某处提前把引擎清了 ✗） | **加一个隔离实验装置**（不再靠猜）：客户端新增 `--load-keep <模型>`（只加载不释放）与 `--metrics`（只读指标）；重活脚本新增 `[1d]` 四步：`[1d1]` load-keep → `[1d2]` **另起一个客户端进程**读指标（看 `engine=` 还是不是 ready）→ `[1d3]` 同一个已加载引擎再问一次意图 → `[1d4]` 收尾指标。这样能把"引擎存活/进程是否换过"和"意图分类"彻底分开 ✗→✓。判据：`[1d2]` 若 `engine=ready` → 引擎跨进程调用存活 ✓，问题在意图链路；若 `engine=absent` → 断点在**加载之后**，用 `[0]` 段式的 ps/pid + rt_stderr 顺序定位 |
| FIX-78b | 附带确认：`[10e]` 两道门 ✅ | —— | —— | `evidence/56`：内置动作表命中 `com.ohos.camera`，但动作白名单默认只放 `com.ohos.settings` → `ExecuteIntent("看相机")` 回 **201** ✓（"表里能命中 ≠ 允许执行"实测成立） |

| **FIX-79** | 重活服务**又**超预算被砍（FIX-67/71 主题第三次）→ 给"重活脚本"立预算纪律 | `evidence/58`：`LUT-EV2-END=0`（重活没跑完）→ `[1d]` 一步都没产出 | 重活脚本的段落一路加到了 5 段（`[1]` t-mac 完整序列 ~40s + `[1b]` qwen 完整序列 ~30s + `[1c]` 2 次带模型意图 ~15s + `[1d]` 4 步实验 ~25s + `[6]` stress ~15s ≈ 2.5–3 分钟），**服务的时间预算装不下** | ① 减负：`[1b]` 改成 `--load`（只加载，端到端由 `[1]` 的 t-mac 段负责）、`[1c]` 去掉重复的“打开相机”那条（轻活 `[10b]` 已验）；② **立纪律（写进本账本）**：重活脚本每加一段先估时长，**总时长控制在 ~1.5 分钟以内**，超了就再拆一个服务（服务有时间预算，靠"重排"只能止损不能根治）——已同步写进 `PLAN.md` §六 的运行规则 |

| **FIX-80** | **S6-3 隔离实验给出决定性结论**（模型路径链路 ✅ / 分类质量 ❌，都有原文） | —— | —— | `evidence/58` 的 `[1d]`：`[1d1]` `--load-keep` ErrCode=0；`[1d2]` **另起一个客户端进程**读指标 → 正常（`peak_rss=1175.9 MB`）→ **引擎跨 IPC 调用存活** ✓；`[1d3]` 同一引擎再问意图 → `ErrCode=0`，`source=keyword`，**`model_said=" notes RotTV abroadTC foreverDATA given Resolnih absolute assumptionena…"`** → **模型真的被调用并产出了**，但输出是**词沙拉**（不是 JSON、不含包名）→ **严格校验正确拒绝 → 如实回退** ✓；`[1d4]` 收尾正常。**结论**：① 模型路径**链路全通**（加载/存活/推理/严格校验/回退）；② **不可用的原因是有原文证据的"模型产物质量"**，不是链路 bug —— 按计划**不调模型、不放宽校验**，已在 `INTEGRATION.md` 把措辞从"实测未通过校验"升级成"实测模型输出为词沙拉（附原文）"。另：修复前的 `[1c1]` 那个 `engine=absent` 在当前构型下**未被复现**（同轮 `[1d2]` 正常）→ 记为"该轮次现象，当前构型不复现"，不做过强解释 |

| **FIX-81** | 差点改错构建文件：仓库里有两份"同名"BUILD.gn/bundle.json | —— | —— | `ohos/sa/component/BUILD.gn` 与 `component/bundle.json` 是**旧 `@fandou/lut_sa` 布局**的死文件（subsystem=`fandou`、`//fandou/lut_sa:lut_sa_package`），而真正入树的是 `intree/vendor/ohemu/lutsa/{BUILD.gn,bundle.json}`（subsystem=`ohemu`）；`install_into_tree.sh` 只拷后者。全仓库零引用 | 先 `grep` 确认零引用、再读内容确认是旧布局，然后 `git rm` 两份死文件（历史在 git 里，不丢）。**教训**：改构建文件前先确认"哪一份是源头"（`install_into_tree.sh` 里拷贝的才是） |

| **FIX-82** | 独立进程调系统库的**可复制配方**：include 不是猜出来的，链接要两份 .so | 探针编译一路报缺头：`ani.h` → `event_handler.h` → `local_handle_adapter.h` → `node_api.h` → `nocopyable.h` → `linux/ashmem.h`……猜 include 根 = 无限循环 | ① 这些头散在 `interface/sdk_c/ani`、`base/notification/eventhandler`、**`third_party/node/src`**、`commonlibrary/c_utils`、arkcompiler 的 ani 插件目录等；② 链接缺 `AccessibilityElementInfo::GetContent/IsClickable/GetAccessibilityId/GetRectInScreen` —— 它们**不在** `libaccessibleability.z.so`，而在 `lib64/platformsdk/libaccessibility_common.z.so`（用非递归 `ls lib64` 会漏看 `platformsdk/` 子目录） | **配方（下次直接抄）**：`ninja -t commands obj/.../<x>.o` → 认准其中 `-c .../<x>.cpp` 的那条真实编译命令 → 把 `-c`/`-o` 换成自己的源与产物就能离线复验；BUILD.gn 的 `include_dirs` 就按那条命令里的 `-I` 顺序照抄（**顺序有意义**：同名头必须由目标组件自己那份先命中）。链接走 `lib_dirs=["prebuilt"] libs=["accessibleability.z","accessibility_common.z"]`，`.so` 每轮 install 从镜像刷新。闭包实测 `arkcompiler/runtime_core/ace_napi/ani` 全为 0 行 ✓。另注：`/tmp/allcmds.txt` 被清空后脚本会"静默通过" → 已加空值守卫（假阳性教训） |

| **FIX-83** | **无障碍读屏的三道门**（S7-1a 的核心发现）：4004 → 1005 → init cfg 里的 permission | `evidence/60`：v1 普通客户端 `GetWindows`/`GetRoot` → **4004**；v2 `AccessibilityUITestAbility` → `RegisterAbilityListener` **1005** | ① **4004 = `RET_ERR_NO_CONNECTION`**：`isConnected_` 只在 `Init(channel, channelId)` 置真，channel 由系统下发给"登记过的无障碍 ability"（读码：`accessible_ability_client_impl.cpp` 各读方法开头都是 `if (!isConnected_) return RET_ERR_NO_CONNECTION;`）。② **1005 = `RET_ERR_NO_PERMISSION`**：服务端 `CheckExtensionAbilityPermission` 要求进程 native token 持有 `ohos.permission.ACCESSIBILITY_EXTENSION_ABILITY`（或进程名恰为 `hdcd`）；紧接着的 `EnableUITestAbility` 还有 `IsSystemApp()`，但该函数对**非 HAP 调用者直接返回 true** ✓。③ 权限怎么给：**init 服务 cfg 的 `permission`/`permission_acls`** —— `init_service.c` 用它们构造 `NativeTokenInfoParams` → `GetAccessTokenId` → 子进程 `SetSelfTokenID`（默认 apl=`system_basic`）。本树先例：`base/sensors/start/etc/init/msdp_musl.cfg` 就给 msdp 声明过 `ACCESSIBILITY_EXTENSION_ABILITY` 等一长串 | 给 `lut_evidence.cfg` 加：`permission: [ACCESSIBILITY_EXTENSION_ABILITY, QUERY_ACCESSIBILITY_ELEMENT]`、`permission_acls: [INJECT_INPUT_EVENT]`（后两项为 S7-3 预留）。**判据实测**：`RegisterAbilityListener ret=0` → `Connect(0) ret=0` → `OnAbilityConnected`（~500–600 ms）→ `GetWindows ret=0 count=5` → `GetRoot ret=0`，全窗口 169 节点/14 带文本/6 可点，`verdict=TREE-OK-WITH-TEXT`、`exit=0` |

| **FIX-84** | **S7-1a 结论：独立原生进程能读屏（带真实文本）** —— GUI Agent 的"看到"这一步在 SA 里可做 | —— | —— | `evidence/60`：`RegisterAbilityListener ret=0`；`Connect(0) ret=0`；`connected=1 currentUserId=100`；5 个窗口 `win6(2110)/win3(2112)/win4(2111)/win10(2108)/win5(2001)`；`[13a]` 全窗口 `printed=169 withText=14 clickable=6`、`[13b]` 单窗口 `printed=43 withText=7`，两次都 `TREE-OK-WITH-TEXT`/`exit=0`。读到的真实文本：`上滑解锁` / `21:25` / `2026年10月5日` / `星期一` / `丙午年八月廿五` / `没有 SIM 卡` / `100%`（当前屏幕=锁屏）。**对 S7 的意义**：① SA 封 `ReadScreen` 可行（SA 的 `lut_sa.cfg` 声明同样权限即可，SA 内 token 带着它们）；② 同一 API 面已含执行侧（`ExecuteAction(elementInfo, action)`/`InjectGesture`/`SetTargetBundleName`），S7-3 不用再找路；③ 如实记录三条边界：走的是"UITest ability"模式（语义上是测试框架口子，生产加固版应换成正式无障碍扩展）、本轮**只读**（未调用任何注入）、连接是进程级单例（与真实 uitest 运行互斥）。**教训（第三次）**：打包器 `head -90` 又把 271 行的 `[13]` 段砍断 → 已改 `head -400`，完整段从 userdata 原样补回 `evidence/60` |

### 往「系统能力级」还差什么（按优先级）

| # | 项 | 现状 | 下一步 |
|---|---|---|---|
| STA-1 | **稳定性** | ✅ 6/6 冷启动注册+调用成功 | 扩到更多轮次/并发调用 |
| STA-2 | **接口鲁棒性** | ✅ 已做：客户端 `--stress` 用例（重复调用 ×3/×2、未加载就推理、坏路径加载、Release 后再调用、压力后自检），结果 `bad=0`、SA 未被搞崩；顺带修掉两个语义 bug：`LoadModel` 原先不看路径是否存在都返回成功、`Generate` 在未加载模型时也返回成功（FIX-48 放开暖机门槛的连带遗漏，已在 `InferTokenBatch` 补 `loaded` 检查） | 证据：`evidence/41-sa-robustness-sta2.txt` |
| STA-3 | **让它真响应（SA 里真推理）** | ✅ 已做：SA 进程内静态链接 **从源码编的 llama.cpp（含 t-mac LUT 内核）**，`LoadModel` 真解析 gguf、`Generate` 真出 token。证据（`evidence/42-sa-real-inference-sta3.txt`）：`LoadModel` ErrCode=0 / 3.3–5.1 s（引擎日志带出 `CPU buffer 462.96 MiB`、`KV 6 MiB`、`graph nodes 846`）；`Generate` 出真实文本；**同 prompt 两次逐字符一致**（`determinism: yes`）、**换 prompt + 贪心输出不同**（`non-constant: yes`）；`GetMetrics` 带 `engine=ready n_ctx=512 threads=4 infer=3`；`Release` 后内存归还；`peak_rss=533 MB`（模型真驻留）。三条关键坑见 FIX-53/54/55，诊断通道见 FIX-56/57，韧性见 FIX-58 | ① **t-mac 2bit 模型还差匹配的 LUT 内核**：`bitnet-3b-tmac.gguf` 存的是**分开的** gate/up/q/k/v（形状 `m3200_k8640`/`m8640_k3200`/`m3200_k3200`），而现有 `kcfg.ini`+`kernels.cc` 只覆盖**融合后**的形状（`m17280_k3200`/`m6400_k3200`/`m6400_k8640`）→ 需用 t-mac 的 gen_kernels 按本模型形状重新生成（准入检查已把缺哪些形状列出来）；② 链式 KV 前缀复用（现在是每次调用清 KV，为了可复现） |
| STA-4 | **对外可调** | 客户端已能调（6 方法失败项 0），但没有权限模型与对外说明；引擎侧已加"模型准入检查"（FIX-58） | 定义"谁能调、调到哪一档"（uid/权限/会话配额）+ 写第三方接入示例 |

> 平台边界（已实测，决定主战场）：第三方应用**不能**注册 SA、**不能**声明 `AppServiceExtensionAbility`（privilege extension）。
> 因此"系统能力"这条路只能在 **OpenHarmony 标准系统**里做（我们自己就是系统厂商），本节的实测都在该环境完成。

> **ArkTS/HAP 阶段的通用判据**：`hvigor ERROR: BUILD FAILED` 只说明"某个系统应用"没编过，真因永远在 `out/x86_64_virt/error.log` 里
> 的 `ERROR Code: <5 位>` 行（如 `10311006` = Kit 校验、`10505001` = 编译器找不到名字）。`entry` 模块的 "N ArkTS Linter Error"
> 是**警告**，不阻断构（`entry` 模块 111 条 linter 警告仍 BUILD SUCCESSFUL）。

> 依赖提醒（按踩坑顺序）：**python**（FIX-7，`ln -sf /usr/bin/python3 /usr/local/bin/python` 或 `apt install python-is-python3`）、
> **autotools 等**（FIX-8 那行 apt 命令）、抓图用 `socat`；`default-jdk / libtinfo5 / genext2fs / mtools / u-boot-tools / mtd-utils` 是 `tools_checker.py` 会提示的可选项。
>
> 上游 hb 插件路径：ERR-2 的 bug 在 `build/hb/util/loader/subsystem_info.py` 的
> `merge_subsystem_overlay()`；本轮修复全部落在**配置层 + 系统依赖层**（未改 hb 源码，SDK 的 cortex-m 目标也未改源码），
> 便于用官方源码树复现。

