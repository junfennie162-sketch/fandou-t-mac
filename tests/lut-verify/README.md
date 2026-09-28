# LUT 内核对照验证工具

诊断 T-MAC 经典静默失败（内核返回 0、无报错、输出全 0/乱码）的 A/B 工具：
真实 HF 权重 → 已知良好输入 + NumPy 参考答案 → 设备上跑内核 → NMSE 对比。

- `gen_testdata_ffn.py` — 生成 A.bin（量化权重）/ B.bin（激活）/ S.bin（scale）/ Cref.bin（参考答案）
- `run_test_dev.cpp` — 跑 preprocessor + 分块 qgemm，打印逐层诊断 + NMSE，可带参考对比

用法与判读见 `docs/ANDROID-FIX.md` §二（三条线索定位法）。

基线成绩（qemu-aarch64 验证）：aarch64 NEON 内核 vs NumPy 参考 **NMSE 8.4e-05**；
x86 修复版 vs 参考 NMSE 1.8e-05。来源：`ohos/ARM64-VALIDATION.md`。

> 注意：`--kcfg`（默认 aarch64-hf-bitnet-3b）必须与你编译进二进制的 kernels.cc 配套，
> 否则必错——这本身就是坑 1 的演示。
