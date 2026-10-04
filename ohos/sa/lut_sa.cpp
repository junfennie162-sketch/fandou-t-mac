#include "lut_sa.h"

#include "qos_policy.h"
#include "session_workspace.h"
#include "tile_compute.h"
#include "tile_pool.h"

// STA-3：推理引擎（llama.cpp + ggml + t-mac LUT 内核）以静态库形式链进 SA。
// 业务层只看这层 C 接口：不涉及 llama 的 C++ 类型，也没有 try/catch（本文件由 GN 编，
// -fno-exceptions），异常在引擎壳内部被捕获并转成错误码。
#include "engine_shim.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace tmac_sa {
namespace {

constexpr int kWarmMLogical = 64;  // → qgemm m=128
constexpr int kWarmK = 3200;
constexpr int kWarmN = 1;
constexpr int kWarmBits = 2;

struct Session {
  bool loaded = false;
  std::string model_path;
  SessionConfig cfg;
  std::unique_ptr<SessionWorkspace> ws;
};

std::mutex g_mu;
std::unordered_map<uint64_t, Session> g_sessions;
std::atomic<uint64_t> g_next{1};

// 单引擎 / 单并发：一次 decode 会一直占着 g_mu 到生成结束。对「一个系统能力服务全机」
// 的语义这是可接受的（调用方排队），多会话并发（共享权重、各自 context）是下一档。
LutEngine* g_engine = nullptr;
std::atomic<int> g_infer_count{0};

// 我们自己的诊断尾巴（引擎日志由引擎壳自己维护，经 lut_engine_last_log() 取）
std::mutex g_diag_mu;
std::string g_diag;
constexpr size_t kDiagCap = 600;

void NoteDiag(const std::string& s) {
  if (s.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lk(g_diag_mu);
  g_diag += "[sa] ";
  g_diag += s;
  g_diag += " ";
  if (g_diag.size() > kDiagCap) {
    g_diag.erase(0, g_diag.size() - kDiagCap);
  }
}

std::string DiagSnapshot() {
  std::lock_guard<std::mutex> lk(g_diag_mu);
  return g_diag;
}

// 调用者须持 g_mu
void EngineFreeUnlocked() {
  if (g_engine != nullptr) {
    lut_engine_free(g_engine);  // mmap 的权重 + KV 缓存还给系统
    g_engine = nullptr;
  }
}

Session* FindUnlocked(uint64_t id) {
  auto it = g_sessions.find(id);
  return it == g_sessions.end() ? nullptr : &it->second;
}

}  // namespace

Status CreateSession(uint64_t* out_session_id) {
  return CreateSessionWithConfig(out_session_id, SessionConfig{});
}

Status CreateSessionWithConfig(uint64_t* out_session_id, const SessionConfig& cfg) {
  if (!out_session_id) {
    return Status::kInvalid;
  }
  auto ws = std::make_unique<SessionWorkspace>();
  if (!ws->Init(cfg.kcfg_path, cfg.n_threads, cfg.act_group_size)) {
    return Status::kKernelFail;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  uint64_t id = g_next.fetch_add(1);
  Session s;
  s.cfg = cfg;
  s.ws = std::move(ws);
  g_sessions.emplace(id, std::move(s));
  *out_session_id = id;
  return Status::kOk;
}

Status LoadModel(uint64_t session_id, const std::string& model_path, int n_threads, int n_ctx) {
  std::lock_guard<std::mutex> lock(g_mu);
  Session* s = FindUnlocked(session_id);
  if (!s) {
    return Status::kNoSession;
  }
  // 路径不可读就明确报错——不该对调用方撒谎说"加载成功"（STA-2 的鲁棒性用例打这条路径）
  if (model_path.empty() || access(model_path.c_str(), R_OK) != 0) {
    NoteDiag("LoadModel: path not readable: " + model_path);
    return Status::kInvalid;
  }
  // 同一份 gguf 重复 LoadModel（调用方常见：改参数再加载）不复用会白吃一份内存
  if (s->loaded && s->model_path == model_path && g_engine != nullptr) {
    return Status::kOk;
  }
  EngineFreeUnlocked();

  const auto t0 = std::chrono::steady_clock::now();
  char err[256] = {0};
  LutEngine* e = lut_engine_load(model_path.c_str(),
                                 n_threads > 0 ? n_threads : s->cfg.n_threads,
                                 n_ctx > 0 ? n_ctx : s->cfg.n_ctx, err, sizeof(err));
  const auto t1 = std::chrono::steady_clock::now();
  if (e == nullptr) {
    NoteDiag(std::string("LoadModel failed: ") + (err[0] ? err : "unknown"));
    s->loaded = false;
    return Status::kInvalid;
  }
  char note[128];
  std::snprintf(note, sizeof(note), "LoadModel ok in %.0f ms (n_ctx=%d threads=%d)",
                std::chrono::duration<double, std::milli>(t1 - t0).count(), lut_engine_n_ctx(e),
                lut_engine_n_threads(e));
  NoteDiag(note);
  g_engine = e;
  s->model_path = model_path;
  s->loaded = true;
  return Status::kOk;
}

Status PrepareWorkspace(uint64_t session_id, int max_k, int max_n) {
  std::lock_guard<std::mutex> lock(g_mu);
  Session* s = FindUnlocked(session_id);
  if (!s || !s->ws) {
    return Status::kNoSession;
  }
  if (!s->ws->EnsureWorkspace(max_k, max_n)) {
    return Status::kNoWorkspace;
  }
  return Status::kOk;
}

Status WarmKernel(uint64_t session_id, int n_threads_override) {
  SessionWorkspace* ws = nullptr;
  int n_threads = 1;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    Session* s = FindUnlocked(session_id);
    if (!s || !s->ws) {
      return Status::kNoSession;
    }
    // 暖机只热身"内核 + 工作区"，与是否已加载模型无关；
    // 原来这里要求 s->loaded，导致 SA 自检（未加载模型时跑内核自检）永远失败。
    // 推理路径（InferTokenBatch）会自己检查 loaded，这里的门不必留着。
    if (!s->ws->EnsureWorkspace(kWarmK, kWarmN)) {
      return Status::kNoWorkspace;
    }
    ws = s->ws.get();
    n_threads = n_threads_override > 0 ? n_threads_override : s->cfg.n_threads;
  }

  tmac_sched::SetThreadQoS(tmac_sched::QoSLevel::kUserInteractive);
  tmac_sched::LogSchedDecision("warm_kernel", tmac_sched::GetThreadQoS());

  // Zero packed weights + shared qlut (already zero from EnsureWorkspace).
  const size_t a_bytes = static_cast<size_t>(kWarmMLogical) * kWarmBits * kWarmK;
  // C: 参考内核按 m = m_logical×bits 行×n 列写出（fp16）→ 缓冲按 m 行分配；
  // 优化内核只累加 m_logical 行，拿到更大缓冲同样安全。
  const size_t c_bytes =
      static_cast<size_t>(kWarmN) * kWarmMLogical * kWarmBits * sizeof(uint16_t);
  std::vector<uint8_t> A(a_bytes, 0);
  // scales: 每逻辑行每 kG=4 组一个 fp16（= m × k/kG × 2 字节；比旧版 4096 更贴合参考语义，宽松兼容优化内核）
  std::vector<uint8_t> scales(static_cast<size_t>(kWarmMLogical) * kWarmBits * (kWarmK / 4) * sizeof(uint16_t), 0);
  std::vector<uint8_t> C(c_bytes, 0);

  // Single-tile warm: full m=64 (multi-tile needs packed-weight row offsets from ggml).
  (void)n_threads;
  TileJob job{};
  job.m_logical = kWarmMLogical;
  job.k = kWarmK;
  job.n = kWarmN;
  job.bits = kWarmBits;
  job.A_tile = A.data();
  job.scales_tile = scales.data();
  job.qlut = ws->qlut();
  job.lut_scales = ws->lut_scales();
  job.lut_biases = ws->lut_biases();
  job.C_tile = C.data();

  int ret = RunTiledQGeMMParallel(&job, 1, 1);
  if (ret != 0) {
    // Fallback: SessionWorkspace compute path
    if (!ws->ComputeTile(A.data(), scales.data(), C.data(), kWarmMLogical, kWarmK, kWarmN,
                         kWarmBits)) {
      return Status::kKernelFail;
    }
  }
  return Status::kOk;
}

Status InferTokenBatch(uint64_t session_id, const char* prompt, char* out_buf, size_t out_cap,
                       int n_predict, double temp, int top_k) {
  if (!prompt || !out_buf || out_cap == 0) {
    return Status::kInvalid;
  }
  out_buf[0] = '\0';

  // 1) LUT 内核暖机（QoS 分档 + 工作区）—— 本 SA 的差异化资产，每次推理前过一遍
  Status st = WarmKernel(session_id, -1);
  if (st != Status::kOk) {
    return st;
  }

  // 2) 引擎就绪性：没加载模型就"生成"在语义上是假的 → 明确报 kNotLoaded
  std::lock_guard<std::mutex> lock(g_mu);
  Session* s = FindUnlocked(session_id);
  if (!s) {
    return Status::kNoSession;
  }
  if (!s->loaded || g_engine == nullptr) {
    return Status::kNotLoaded;
  }

  // 3) 真推理（引擎壳内部完成 tokenize → decode → 采样 → detokenize）
  char err[256] = {0};
  const int rc = lut_engine_generate(g_engine, prompt, n_predict, temp, top_k, out_buf, out_cap,
                                     err, sizeof(err));
  g_infer_count.fetch_add(1);
  if (rc != 0) {
    NoteDiag(std::string("Generate failed: ") + (err[0] ? err : "unknown"));
    return Status::kKernelFail;
  }
  return Status::kOk;
}

Status ReleaseSession(uint64_t session_id) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_sessions.find(session_id);
  if (it == g_sessions.end()) {
    return Status::kNoSession;
  }
  EngineFreeUnlocked();
  g_sessions.erase(it);
  return Status::kOk;
}

bool EngineReady() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_engine != nullptr;
}

std::string EngineInfo() {
  std::string model_path;
  char buf[320];
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (const auto& kv : g_sessions) {
      if (kv.second.loaded) {
        model_path = kv.second.model_path;
        break;
      }
    }
    std::snprintf(buf, sizeof(buf), "engine=%s n_ctx=%d threads=%d infer=%d model=%s",
                  g_engine != nullptr ? "ready" : "absent",
                  g_engine != nullptr ? lut_engine_n_ctx(g_engine) : 0,
                  g_engine != nullptr ? lut_engine_n_threads(g_engine) : 0,
                  g_infer_count.load(), model_path.c_str());
  }
  std::string out = buf;
  const std::string diag = DiagSnapshot();
  if (!diag.empty()) {
    out += " | diag: ";
    out += diag;
  }
  const char* elog = lut_engine_last_log();
  if (elog != nullptr && elog[0] != '\0') {
    out += " | llama_log: ";
    out += elog;
  }
  return out;
}

}  // namespace tmac_sa
