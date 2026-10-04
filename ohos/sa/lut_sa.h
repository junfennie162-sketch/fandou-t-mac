#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tmac_sa {

enum class Status : int32_t {
  kOk = 0,
  kInvalid = 1,
  kNoSession = 2,
  kNotLoaded = 3,
  kKernelFail = 4,
  kNoWorkspace = 5,
};

struct SessionConfig {
  int n_threads = 4;
  int act_group_size = 32;
  int n_ctx = 512;        // STA-3：引擎上下文长度（= KV 容量上限）
  std::string kcfg_path;  // empty → TMAC_KCFG_FILE
};

// IPC-shaped API (feasibility P3). samgr binding is separate; these are the SA entrypoints.
Status CreateSession(uint64_t* out_session_id);
Status CreateSessionWithConfig(uint64_t* out_session_id, const SessionConfig& cfg);
// STA-3：真加载 gguf（SA 进程内静态链接 llama.cpp）。n_threads/n_ctx <= 0 时用 SessionConfig。
Status LoadModel(uint64_t session_id, const std::string& model_path, int n_threads = 0, int n_ctx = 0);
Status PrepareWorkspace(uint64_t session_id, int max_k, int max_n);
// Warm decode GEMV path: zero weights, shape m=128,k=3200,n=1,b=2 (bitnet tile).
Status WarmKernel(uint64_t session_id, int n_threads_override = -1);
// STA-3：真推理。temp<=0 → 贪心（确定性）；否则 top-k + 温度（固定种子，可复现）。
// 输出为 detokenize 后的文本（截断到 out_cap，含 '\0'）；未加载模型返回 kNotLoaded。
Status InferTokenBatch(uint64_t session_id, const char* prompt, char* out_buf, size_t out_cap,
                       int n_predict = 24, double temp = 0.0, int top_k = 40);
Status ReleaseSession(uint64_t session_id);

// 引擎状态：借 GetMetrics 这个"能通的通道"把诊断带出去（FIX-49）
bool EngineReady();
std::string EngineInfo();

}  // namespace tmac_sa
