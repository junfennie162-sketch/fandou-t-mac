# LUT-SA 在 OpenHarmony 标准系统镜像（QEMU x86_64_virt）里的点亮证据

产物：`out/x86_64_virt/packages/phone/images/{system,vendor,userdata,sys_prod,chip_prod,updater,ramdisk}.img` + `bzImage`

## 一、SA 注册（点亮）—— 见 `11-sa-boot-traces.txt`

```
[ 3.036] [pid=1][Init] ServiceStart starting:lut_sa        ← init 按 /system/etc/init/lut_sa.cfg 拉起
[ 3.039] ServiceExec lut_sa                                 ← exec /system/bin/sa_main + /system/profile/lut_sa.json
[ 3.253] SAMGR: SA:6901 OpenSo 0ms                          ← samgr 打开 /system/lib64/libtmac_sa.z.so
[ 3.258] SAFWK: Publish SA:6901 result:1,spend:2ms          ← ★ 发布成功，登记进 samgr
[ 3.260] SAFWK: Start OnStart-SA:6901 finished, spend:4 ms   ← ★ OnStart() 执行完成
```

## 二、SA 可被调用（IPC 打通）—— 见 `21-ipc-call-traces.txt` / `22-console-evidence-ipc.txt`

另一个进程（`/system/bin/lut_sa_client`，secon=init 域）经 samgr 拿到 6901 并**跨 IPC 调用成功**：

```
self context: u:r:init:s0                                    ← SELinux 策略已加载、域正确
[GetSystemAbility(6901)] ok, remote=0x7ff0e5cfccb0           ← ★ 拿到远端对象
[iface_cast] ok —— IPC 代理已建立
[NativeVersion] ErrCode=0 -> LUT-SA native | SA mode | tfloat=2 B | kcfg embedded | static llama.cpp + LUT kernels
[GetMetrics]    ErrCode=0 -> peak_rss=7.7 MB, session=1, sa_id=6901
```

（`SelfTest` 返回 ErrCode=22，属参考内核自检参数问题，与 IPC 通道无关）

## 三、镜像内文件

| 路径 | 大小 | 作用 |
|---|---|---|
| `/system/lib64/libtmac_sa.z.so` | 85,312 B | SA 实现（含 `LutSystemAbility::OnStart/OnStop/OnSvcCmd` 等 24 个符号） |
| `/system/bin/lut_sa_client` | 25,384 B | IPC 客户端（自证 SA 可被调用） |
| `/system/profile/lut_sa.json` | 276 B | SA 注册表：SA_ID 6901 → libtmac_sa.z.so |
| `/system/etc/init/lut_sa.cfg` | 312 B | init 启动项 |
| `/system/etc/init/lut_evidence.cfg` + `/system/bin/lut_evidence.sh` | — | 开机一次性取证（串口通道） |

## 四、本环境的取证通道说明

`hidumper` 二进制在本 QEMU 环境自身即失败（`-h/-ls/-s` 全部 exit=3、0 字节输出），`hdcd` 启动约 26 s 被 watchdog SIGKILL（hdc 连不上），因此取证走**串口控制台**：镜像内加了一个 `start-mode: boot` 的一次性服务把结果打到 `/dev/ttyS0`。

## 五、文件清单

| 文件 | 内容 |
|---|---|
| `10-serial-full-with-publish.log` / `20-serial-with-ipc-call.log` | 完整串口日志（分别含"点亮"与"IPC 调用"证据） |
| `11-sa-boot-traces.txt` | SA 注册链关键行 |
| `21-ipc-call-traces.txt` | IPC 调用链关键行 |
| `22-console-evidence-ipc.txt` | guest 内取证输出（SELinux 状态 / 客户端调用结果 / ps / 文件 / hilog） |
| `12-console-evidence.txt` | 早期取证输出（无策略时的对照） |
| `04-ps-lut_sa.txt`、`05-files.txt` | 进程与文件快照 |
| `40-sa-stability-6rounds.txt` | STA-1：6/6 轮冷启动全绿 |
| `41-sa-robustness-sta2.txt` | STA-2：`--stress` 鲁棒性用例 `bad=0` |
| `42-sa-real-inference-sta3.txt` | **STA-3：SA 进程内真推理**（真 `LoadModel` + 真出 token + 可复现 + 模型准入拒绝 + 引擎运行期日志） |

## 六、真 · 调优 LUT 内核已接入（本节为最新一轮）

原先 SA 里跑的是**自包含参考内核**（`lut_kernel_ref.cpp`，标量）。现已把仓库里 t-mac 引擎对 x86_64 调优出的
`deploy/tuned/ohos-x64-bitnet-3b/kernels.cc`（1709 行，AVX2）编进 SA（`BUILD.gn` 按 `target_cpu` 选内核，
x86_64 加 `-mavx2`；arm64 走 `deploy/tuned/aarch64-hf-bitnet-3b/`），参考实现入口改名 `ref_*` 退居"算法自检锚点"。

库体积 85,312 → **98,600 字节**，新增 6 个调优内核符号：

```
qgemm_lut_t1_int8_m128_k3200_n1_b2      ← 暖机形状正好命中这条
qgemm_lut_t1_int8_m256_k3200_n1_b2
qgemm_lut_t1_int8_m256_k8640_n1_b2
preprocessor_t1_int8_m6400_k3200_n1_b2
preprocessor_t1_int8_m17280_k3200_n1_b2
preprocessor_t1_int8_m6400_k8640_n1_b2
```

并且 `SelfTest` 现在跑的就是真内核（不再需要加载模型）：

```
[SelfTest] ErrCode=0 -> PASS: LUT kernel warm-up (m128-k3200 b2, 调优内核, 167.1 us)
                     | PASS: ref LUT kernel numeric check (k=8 bits=2 got=8.000 expect=8.0)
---- 客户端调用汇总：失败项 0 ----
```

## 七、客户端调用的 6 个 IPC 方法（全部打通）

| 方法 | 结果 |
|---|---|
| `NativeVersion` | ErrCode=0，返回内核版本串 |
| `SelfTest` | ErrCode=0，**真内核暖机 PASS + 参考内核数值锚点 PASS** |
| `GetMetrics` | ErrCode=0，返回 peak_rss / session / sa_id / 自检详情 |
| `LoadModel` / `Generate` / `Release` | 需模型路径（本次未带；未加载模型时 `Generate` 预期返回错误码） |
