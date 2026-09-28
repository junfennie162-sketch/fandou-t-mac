# 截图证据索引

## device/ —— 真机实测（HUAWEI HBN-AL00 · HarmonyOS 6.1.1 · arm64 · 2026-09-27）

App 控制台会话连续截图（按时间排序）+ 桌面安装证据。本次会话的实测成绩
（`llama_perf_context_print` 屏幕原始输出）：

```
load time       =  1955.44 ms
prompt eval     =   239.87 ms /   7 tokens (  34.27 ms/token,  29.18 tokens per second)
eval time       =   859.51 ms /  16 runs   (  53.72 ms/token,  18.62 tokens per second)
```

| 文件 | 内容 |
|---|---|
| device-console-01.jpg ~ device-console-09.jpg | App 控制台会话：构建信息（arm64 NEON fp16）→ 模型加载 verbose 输出 → 生成结果（`The capital of china is Beijing…`）→ 性能计时块 |
| device-home-icon.jpg | **真机桌面**：LUT-SA 应用图标已安装（部署完成证据） |

> 说明：零售真机 hilog 被 SELinux 禁用，性能数据靠 App 内置屏幕控制台显示、截屏读取（方案见 `../FULL-REPORT.md` §6.4）。
> 原始文件存在 1 组字节级重复（已去重），共 10 张。

## emulator/ —— 模拟器实测（HarmonyOS 7.0.0 · x86_64 · 2026-09-26/27）

| 文件 | 内容 |
|---|---|
| hap-demo-screenshot.png | 完整推理成功：`the city of Paris…`，eval ≈10.5 tok/s（三次交叉验证 10.38/10.51/10.52） |
| hap-demo-prompt.png | 输入 prompt 的界面状态 |
| hap-console-style.png | 控制台风格输出（与 CLI 同格式，便于对拍） |
| hap-picker-screenshot.png | 系统文件选择器导入模型（隐私提示 + .gguf 过滤） |
