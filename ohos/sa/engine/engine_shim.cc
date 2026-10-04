// engine_shim.cc —— 引擎壳：把 llama.cpp 包成「不抛异常的 C 接口」
//
// 编在引擎静态库里（由 intree/build_engine.sh 用 OH 树自己的 clang 编，带 -fexceptions），
// 与 llama.cpp/ggml/kernels.cc 同一个 .a。业务层（GN，-fno-exceptions）只看 engine_shim.h。
#include "engine_shim.h"

#include "llama.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <random>
#include <string>
#include <vector>

struct LutEngine {
  llama_model* model = nullptr;
  llama_context* ctx = nullptr;
  int n_ctx = 0;
  int n_threads = 0;
};

namespace {

std::mutex g_log_mu;
std::string g_log;
constexpr size_t kLogCap = 1200;

void AppendLog(const std::string& s) {
  std::lock_guard<std::mutex> lk(g_log_mu);
  g_log += s;
  if (g_log.size() > kLogCap) {
    g_log.erase(0, g_log.size() - kLogCap);
  }
}

void LogCapture(ggml_log_level /*level*/, const char* text, void* /*user*/) {
  if (text != nullptr) {
    AppendLog(text);
  }
}

void SetErr(char* err, size_t cap, const std::string& msg) {
  if (err == nullptr || cap == 0) {
    return;
  }
  std::snprintf(err, cap, "%s", msg.c_str());
}

// 阶段标记：加载/生成很慢时，"卡在哪一步"必须能从 SA 的 stderr 文件里看出来
void Trace(const std::string& s) {
  AppendLog(std::string("[shim] ") + s + "\n");
  std::fprintf(stderr, "[shim] %s\n", s.c_str());
  std::fflush(stderr);
}

// 采样：temp<=0 → 贪心（确定性，系统能力的默认期望）；否则 top-k + 温度。
// 重复惩罚与 App 侧同一语义：正 logits 除、负 logits 乘。
llama_token SampleNext(float* logits, int n_vocab, double temp, int top_k,
                       const std::vector<llama_token>& recent, std::mt19937& rng) {
  if (temp <= 0.0 || top_k == 1) {
    int best = 0;
    for (int i = 1; i < n_vocab; ++i) {
      if (logits[i] > logits[best]) {
        best = i;
      }
    }
    return static_cast<llama_token>(best);
  }

  constexpr float kRepPenalty = 1.15f;
  constexpr int kRepLastN = 64;
  const int n_rep = std::min(static_cast<int>(recent.size()), kRepLastN);
  for (int i = 0; i < n_rep; ++i) {
    const llama_token t = recent[recent.size() - 1 - static_cast<size_t>(i)];
    if (t < 0 || t >= n_vocab) {
      continue;
    }
    logits[t] = logits[t] > 0 ? logits[t] / kRepPenalty : logits[t] * kRepPenalty;
  }

  const int k = std::min(top_k > 0 ? top_k : 40, n_vocab);
  std::vector<int> idx(static_cast<size_t>(n_vocab));
  for (int i = 0; i < n_vocab; ++i) {
    idx[static_cast<size_t>(i)] = i;
  }
  std::partial_sort(idx.begin(), idx.begin() + k, idx.end(),
                    [logits](int a, int b) { return logits[a] > logits[b]; });

  std::vector<double> p(static_cast<size_t>(k));
  const double mx = logits[idx[0]];
  double sum = 0.0;
  for (int i = 0; i < k; ++i) {
    p[static_cast<size_t>(i)] = std::exp((logits[idx[static_cast<size_t>(i)]] - mx) / temp);
    sum += p[static_cast<size_t>(i)];
  }
  if (sum <= 0.0) {
    return static_cast<llama_token>(idx[0]);
  }
  std::uniform_real_distribution<double> u(0.0, 1.0);
  const double r = u(rng) * sum;
  double acc = 0.0;
  for (int i = 0; i < k; ++i) {
    acc += p[static_cast<size_t>(i)];
    if (r <= acc) {
      return static_cast<llama_token>(idx[static_cast<size_t>(i)]);
    }
  }
  return static_cast<llama_token>(idx[0]);
}

}  // namespace

extern "C" LutEngine* lut_engine_load(const char* model_path, int n_threads, int n_ctx, char* err,
                                      size_t err_cap) {
  if (model_path == nullptr) {
    SetErr(err, err_cap, "null model path");
    return nullptr;
  }
  try {
    // 准入检查：形状表对不上就直接拒绝，别让 ggml-tmac 的 LOG(FATAL) 把 SA 进程打掉
    {
      char why[512] = {0};
      if (lut_engine_admission_check(model_path, "/system/etc/lut_sa/kcfg.ini", why, sizeof(why)) != 0) {
        Trace(std::string("load: rejected by admission: ") + why);
        SetErr(err, err_cap, why);
        return nullptr;
      }
    }
    Trace("load: backend_init");
    llama_backend_init();  // 幂等
    llama_log_set(LogCapture, nullptr);

    llama_model_params mp = llama_model_default_params();
    mp.use_mmap = true;  // 966 MB 权重走 mmap：不必一次性读进堆，页缓存按需装填
    mp.n_gpu_layers = 0;

    Trace(std::string("load: llama_load_model_from_file ") + model_path);
    llama_model* m = llama_load_model_from_file(model_path, mp);
    if (m == nullptr) {
      SetErr(err, err_cap, "llama_load_model_from_file returned null");
      return nullptr;
    }
    Trace("load: model ok");

    const int want_ctx = n_ctx > 0 ? n_ctx : 512;
    const int want_thr = n_threads > 0 ? n_threads : 4;
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = static_cast<uint32_t>(want_ctx);
    cp.n_batch = 512;
    cp.n_threads = want_thr;
    cp.n_threads_batch = want_thr;
    // KV 保持 f16：BitNet-3B 的 head_dim=100 不是 32 的整数倍，量化 KV（32 元素块）会撞
    // llama_new_context_with_model 里的 assert（App 侧实测 abort）—— 这里不碰 type_k/type_v
    cp.type_k = GGML_TYPE_F16;
    cp.type_v = GGML_TYPE_F16;
    cp.flash_attn = false;

    Trace("load: new_context");
    llama_context* c = llama_new_context_with_model(m, cp);
    if (c == nullptr) {
      llama_free_model(m);
      SetErr(err, err_cap, "llama_new_context_with_model returned null");
      return nullptr;
    }
    Trace("load: ready");

    auto* e = new LutEngine();
    e->model = m;
    e->ctx = c;
    e->n_ctx = want_ctx;
    e->n_threads = want_thr;
    return e;
  } catch (const std::exception& ex) {
    SetErr(err, err_cap, std::string("exception: ") + ex.what());
    return nullptr;
  } catch (...) {
    SetErr(err, err_cap, "unknown exception");
    return nullptr;
  }
}

extern "C" int lut_engine_generate(LutEngine* e, const char* prompt, int n_predict, double temp,
                                   int top_k, char* out, size_t out_cap, char* err,
                                   size_t err_cap) {
  if (e == nullptr || e->model == nullptr || e->ctx == nullptr) {
    SetErr(err, err_cap, "engine not loaded");
    return 1;
  }
  if (prompt == nullptr || out == nullptr || out_cap == 0) {
    SetErr(err, err_cap, "bad args");
    return 1;
  }
  out[0] = '\0';

  llama_model* model = e->model;
  llama_context* ctx = e->ctx;
  const int n_vocab = llama_n_vocab(model);
  const int n_ctx = e->n_ctx;

  try {
    const auto prompt_len = static_cast<int32_t>(std::strlen(prompt));
    if (prompt_len <= 0) {
      SetErr(err, err_cap, "empty prompt");
      return 1;
    }
    Trace("gen: tokenize");

    // tokenize（返回负数说明缓冲不够，按 -n 扩容重试）
    std::vector<llama_token> tokens(static_cast<size_t>(prompt_len) + 8);
    int n_tok = llama_tokenize(model, prompt, prompt_len, tokens.data(),
                               static_cast<int32_t>(tokens.size()), true, false);
    if (n_tok < 0) {
      tokens.resize(static_cast<size_t>(-n_tok));
      n_tok = llama_tokenize(model, prompt, prompt_len, tokens.data(),
                             static_cast<int32_t>(tokens.size()), true, false);
    }
    if (n_tok <= 0) {
      SetErr(err, err_cap, "tokenize failed");
      return 1;
    }
    if (n_tok >= n_ctx - 1) {
      SetErr(err, err_cap, "prompt longer than n_ctx");
      return 1;
    }
    tokens.resize(static_cast<size_t>(n_tok));

    // 每次调用清 KV：同一 prompt 的结果可复现（链式 KV 前缀复用见 README「下一档」）
    llama_kv_cache_clear(ctx);
    Trace("gen: prompt decode");
    if (llama_decode(ctx, llama_batch_get_one(tokens.data(), n_tok, 0, 0)) != 0) {
      SetErr(err, err_cap, "prompt decode failed");
      return 1;
    }
    Trace("gen: sampling");

    // 逐 token 生成：logits 取最后一个 token 那行（logits_all=false 时缓冲只有这一行）
    const llama_token eos = llama_token_eos(model);
    const int want = n_predict > 0 ? n_predict : 24;
    std::mt19937 rng(42);  // 固定种子 → 采样路径也可复现
    std::vector<llama_token> recent = tokens;
    std::string text;
    int n_past = n_tok;
    for (int i = 0; i < want && n_past < n_ctx - 1; ++i) {
      float* logits = llama_get_logits(ctx);
      if (logits == nullptr) {
        break;
      }
      llama_token id = SampleNext(logits, n_vocab, temp, top_k, recent, rng);
      if (id == eos) {
        break;
      }
      char piece[256];
      const int np = llama_token_to_piece(model, id, piece, sizeof(piece), 0, false);
      if (np > 0) {
        text.append(piece, static_cast<size_t>(np));
      }
      if (llama_decode(ctx, llama_batch_get_one(&id, 1, n_past, 0)) != 0) {
        break;
      }
      recent.push_back(id);
      ++n_past;
    }
    std::snprintf(out, out_cap, "%s", text.c_str());
    char note[96];
    std::snprintf(note, sizeof(note), "gen: done %d tokens", n_past - n_tok);
    Trace(note);
    return 0;
  } catch (const std::exception& ex) {
    SetErr(err, err_cap, std::string("exception: ") + ex.what());
    return 1;
  } catch (...) {
    SetErr(err, err_cap, "unknown exception");
    return 1;
  }
}

extern "C" void lut_engine_free(LutEngine* e) {
  if (e == nullptr) {
    return;
  }
  try {
    if (e->ctx != nullptr) {
      llama_free(e->ctx);
    }
    if (e->model != nullptr) {
      llama_free_model(e->model);
    }
  } catch (...) {
    // 释放路径不该抛，兜住即可
  }
  delete e;
}

extern "C" int lut_engine_n_ctx(const LutEngine* e) {
  return e == nullptr ? 0 : e->n_ctx;
}

extern "C" int lut_engine_n_threads(const LutEngine* e) {
  return e == nullptr ? 0 : e->n_threads;
}

extern "C" const char* lut_engine_last_log(void) {
  static thread_local std::string snapshot;
  std::lock_guard<std::mutex> lk(g_log_mu);
  snapshot = g_log;
  for (auto& c : snapshot) {
    if (c == '\n' || c == '\r') {
      c = ' ';  // 单行塞进 IPC 结果串
    }
  }
  return snapshot.c_str();
}
