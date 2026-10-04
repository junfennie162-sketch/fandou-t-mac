// engine_shim.h —— 引擎（llama.cpp + ggml + t-mac LUT 内核）对 LUT-SA 的唯一接口
//
// 为什么要这层壳（STA-3 定论）：
//   1) OH 标准系统的 GN 工具链强制 -fno-exceptions，而这份 llama.cpp 快照用 `throw
//      std::runtime_error` 报错（坏 gguf / 不支持的张量…）→ 引擎不可能在 GN 里编，
//      改为用 OH 自己的 clang 编成静态库（intree/build_engine.sh，带 -fexceptions）。
//   2) 业务层（GN 编，-fno-exceptions）不能出现 try/catch，也不该被异常穿透 →
//      所有异常在这层壳内部被捕获，转成错误码 + 错误串。
//   3) 引擎类型完全不外泄（不透明句柄），SA 只依赖这两个头 → 换引擎不动业务层。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LutEngine LutEngine;  // 不透明句柄

// 模型准入检查（加载前拦一道）：按 gguf 格式读出量化张量形状，与 LUT 形状表(kcfg)比对。
// 返回 1 = 拒绝（原因写进 err），0 = 放行/无法判定。ggml-tmac 在加载期查不到 kcfg 会
// LOG(FATAL)（原实现 abort()），对系统能力来说"先拦一道"比"进程被打死"好得多。
int lut_engine_admission_check(const char* model_path, const char* kcfg_path, char* err,
                               size_t err_cap);

// 加载 gguf。成功返回非空句柄；失败返回 NULL 并把原因写进 err。
// n_threads/n_ctx <= 0 → 用默认（4 线程 / 512 上下文）。
LutEngine* lut_engine_load(const char* model_path, int n_threads, int n_ctx, char* err, size_t err_cap);

// 生成文本。temp<=0 → 贪心（确定性）；否则 top-k + 温度（固定种子 42）。
// out 以 '\0' 结尾（超长截断）。返回 0 成功，非 0 失败并把原因写进 err。
int lut_engine_generate(LutEngine* e, const char* prompt, int n_predict, double temp, int top_k,
                        char* out, size_t out_cap, char* err, size_t err_cap);

// 释放模型与上下文（mmap 的权重 + KV 缓存还给系统）。NULL 安全。
void lut_engine_free(LutEngine* e);

// 已加载引擎的上下文长度 / 线程数（未加载返回 0）
int lut_engine_n_ctx(const LutEngine* e);
int lut_engine_n_threads(const LutEngine* e);

// 引擎日志尾巴（llama.cpp 的 log 回调内容 + 我们自己的诊断）——借能通的通道带出去
const char* lut_engine_last_log(void);

#ifdef __cplusplus
}
#endif
