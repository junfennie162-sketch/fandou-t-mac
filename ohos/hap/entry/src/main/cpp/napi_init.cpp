// LUT-SA HAP NAPI bridge — T-MAC LLM inference inside a HarmonyOS app.
//
// ArkTS side:  import tmac from 'libtmac_hap.so'
// Exposed (sync):
//   nativeVersion()                                        -> string
//   selfTest()                                             -> string   (LUT kernel: zero-in => zero-out)
//   bench(steps)                                           -> string   (LUT kernel micro-benchmark)
//   loadModel(path, filesDir, threads, nCtx, useMmap)       -> string
//   generate(prompt, nPredict, temp, topK)                  -> string   (text + tok/s timings)
//   release()                                              -> string
// Exposed (async, Promise — use these from the UI so the app stays responsive):
//   loadModelAsync(path, filesDir, threads, nCtx, useMmap)  -> Promise<string>
//   generateAsync(prompt, nPredict, temp, topK)             -> Promise<string>
//
// The static libs linked into this .so (per ABI, see CMakeLists.txt) already contain the
// T-MAC LUT kernels. The kcfg that matches those kernels is embedded below and written into
// the app sandbox on first use; TMAC_KCFG_FILE is then setenv'd — the T-MAC wrapper checks
// the environment variable first, so the build-machine path baked into the libs never matters.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "napi/native_api.h"
#include "llama.h"
#include "t-mac/kernels.h"

#if defined(__x86_64__)
// fp32 scalars (AVX2 build), matching staging-x64 kernels
#define TMAC_TFLOAT_BYTES 4
static const char *kKcfgIni = R"KCFG([qgemm_lut_t1_int8_m6400_k8640_n1_b2]
bm = 256
simd_n_in = 16
simd_n_out = 8
kfactor = 16
group_size = 128
lut_scales_size = 135
scales_size = 1
n_tile_num = 25

[qgemm_lut_t1_int8_m17280_k3200_n1_b2]
bm = 128
simd_n_in = 16
simd_n_out = 8
kfactor = 16
group_size = 128
lut_scales_size = 50
scales_size = 1
n_tile_num = 135

[qgemm_lut_t1_int8_m6400_k3200_n1_b2]
bm = 256
simd_n_in = 16
simd_n_out = 8
kfactor = 16
group_size = 128
lut_scales_size = 50
scales_size = 1
n_tile_num = 25
)KCFG";
#else
// fp16 scalars (NEON build), matching staging-arm64 kernels
#define TMAC_TFLOAT_BYTES 2
static const char *kKcfgIni = R"KCFG([qgemm_lut_t1_int8_m6400_k8640_n1_b2]
bm = 128
simd_n_in = 16
simd_n_out = 8
kfactor = 16
group_size = 128
lut_scales_size = 135
scales_size = 1
n_tile_num = 50

[qgemm_lut_t1_int8_m17280_k3200_n1_b2]
bm = 128
simd_n_in = 16
simd_n_out = 8
kfactor = 16
group_size = 128
lut_scales_size = 50
scales_size = 1
n_tile_num = 135

[qgemm_lut_t1_int8_m6400_k3200_n1_b2]
bm = 320
simd_n_in = 16
simd_n_out = 8
kfactor = 16
group_size = 128
lut_scales_size = 50
scales_size = 1
n_tile_num = 20
)KCFG";
#endif

namespace {

// ---------------------------------------------------------------- kernel self-test / bench
// Shape shared by both arch kernel sets: m_bits=128, k=3200, n=1, bits=2 -> 64 output rows.
constexpr int kMBits = 128;
constexpr int kK = 3200;
constexpr int kN = 1;
constexpr int kBits = 2;
constexpr int kOutRows = kMBits / kBits;   // 64

void *alloc64(size_t nbytes) {
    void *p = nullptr;
    if (posix_memalign(&p, 64, nbytes) != 0 || !p) {
        return nullptr;
    }
    std::memset(p, 0, nbytes);
    return p;
}

struct KernelBuffers {
    void *A = nullptr;
    void *qlut = nullptr;
    void *scales = nullptr;
    void *lut_scales = nullptr;
    void *lut_biases = nullptr;
    void *C = nullptr;

    size_t c_bytes() const { return (size_t) kOutRows * TMAC_TFLOAT_BYTES; }

    bool ok() const { return A && qlut && scales && lut_scales && lut_biases && C; }

    ~KernelBuffers() {
        std::free(A); std::free(qlut); std::free(scales);
        std::free(lut_scales); std::free(lut_biases); std::free(C);
    }
};

KernelBuffers make_kernel_buffers() {
    KernelBuffers b;
    b.A          = alloc64((size_t) kMBits * kK / 8);            // 51200 B
    b.qlut       = alloc64((size_t) kK / 4 * 16);                // 12800 B
    b.scales     = alloc64(256 * TMAC_TFLOAT_BYTES);             // kernels index up to 120+
    b.lut_scales = alloc64((size_t) kK / 64 * TMAC_TFLOAT_BYTES);
    b.lut_biases = alloc64((size_t) kK / 64 * TMAC_TFLOAT_BYTES);
    b.C          = alloc64((size_t) kOutRows * TMAC_TFLOAT_BYTES);
    return b;
}

bool all_zero(const void *p, size_t n) {
    const uint8_t *q = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; ++i) {
        if (q[i] != 0) return false;
    }
    return true;
}

int run_kernel_once(KernelBuffers &b) {
    std::memset(b.C, 0x5a, b.c_bytes());   // must be overwritten by the kernel
    return qgemm_lut_int8(kMBits, kK, kN, kBits, b.A, b.qlut, b.scales, b.lut_scales, b.lut_biases, b.C);
}

std::string run_self_test() {
    KernelBuffers b = make_kernel_buffers();
    if (!b.ok()) return "FAIL: buffer allocation";
    char local[320];
    std::snprintf(local, sizeof(local), "kernel m=%d k=%d n=%d b=%d (%s) | ", kMBits, kK, kN, kBits,
#if defined(__x86_64__)
                  "x86_64 AVX2"
#else
                  "arm64 NEON"
#endif
    );
    std::string out = local;
    const int ret = run_kernel_once(b);
    if (ret != 0) {
        std::snprintf(local, sizeof(local), "FAIL: qgemm_lut_int8 returned %d", ret);
        return out + local;
    }
    if (!all_zero(b.C, b.c_bytes())) return out + "FAIL: zero input produced non-zero output";
    return out + "PASS: LUT kernel ran, zero-in => zero-out";
}

std::string run_bench(int steps) {
    if (steps <= 0 || steps > 100000) return "FAIL: steps must be in [1, 100000]";
    KernelBuffers b = make_kernel_buffers();
    if (!b.ok()) return "FAIL: buffer allocation";
    if (run_kernel_once(b) != 0) return "FAIL: kernel error during warmup";
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < steps; ++i) {
        if (run_kernel_once(b) != 0) return "FAIL: kernel error during bench";
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    char local[320];
    std::snprintf(local, sizeof(local),
                  "kernel m=%d k=%d n=%d b=%d | steps=%d | total=%.2f ms | avg=%.4f ms",
                  kMBits, kK, kN, kBits, steps, total_ms, total_ms / steps);
    return local;
}

// ---------------------------------------------------------------- model / context state
struct LlmState {
    llama_model *model = nullptr;
    llama_context *ctx = nullptr;
    std::string kcfg_path;
    std::string model_path;
    int n_threads = 4;
    int n_ctx = 512;
    double load_ms = 0.0;
};

LlmState g_llm;

bool ensure_kcfg(const std::string &files_dir) {
    if (!g_llm.kcfg_path.empty() && std::getenv("TMAC_KCFG_FILE")) return true;
    if (files_dir.empty()) return false;
    const std::string path = files_dir + "/kcfg.ini";
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(kKcfgIni, 1, std::strlen(kKcfgIni), f);
    std::fclose(f);
    setenv("TMAC_KCFG_FILE", path.c_str(), 1);
    g_llm.kcfg_path = path;
    return true;
}

void release_llm() {
    if (g_llm.ctx) { llama_free(g_llm.ctx); g_llm.ctx = nullptr; }
    if (g_llm.model) { llama_free_model(g_llm.model); g_llm.model = nullptr; }
    g_llm.model_path.clear();
    g_llm.load_ms = 0.0;
}

// top-k / temperature sampler (self-contained: this fork keeps its samplers in common/,
// which is not linked into the HAP)
llama_token sample_top_k(float *logits, int n_vocab, int top_k, float temp, std::mt19937 &rng) {
    if (top_k <= 0 || top_k > n_vocab) top_k = n_vocab;
    std::vector<int> idx(n_vocab);
    for (int i = 0; i < n_vocab; ++i) idx[i] = i;
    if (top_k < n_vocab) {
        std::nth_element(idx.begin(), idx.begin() + top_k, idx.end(),
                         [&](int a, int b) { return logits[a] > logits[b]; });
        idx.resize(top_k);
    }
    float maxl = -INFINITY;
    for (int i : idx) maxl = std::max(maxl, logits[i]);
    std::vector<double> p(idx.size());
    double sum = 0.0;
    for (size_t i = 0; i < idx.size(); ++i) {
        p[i] = std::exp((double) (logits[idx[i]] - maxl) / (double) temp);
        sum += p[i];
    }
    std::uniform_real_distribution<double> dist(0.0, sum);
    double r = dist(rng);
    double acc = 0.0;
    for (size_t i = 0; i < idx.size(); ++i) {
        acc += p[i];
        if (r <= acc) return idx[i];
    }
    return idx.back();
}

napi_value make_string(napi_env env, const std::string &s) {
    napi_value v = nullptr;
    napi_create_string_utf8(env, s.c_str(), s.length(), &v);
    return v;
}

std::string get_string_arg(napi_env env, napi_value v) {
    size_t len = 0;
    napi_get_value_string_utf8(env, v, nullptr, 0, &len);
    std::string s(len, '\0');
    napi_get_value_string_utf8(env, v, &s[0], len + 1, &len);
    return s;
}

// ---------------------------------------------------------------- core work (runs on the JS thread for the
// sync entry points, on a NAPI worker thread for the Async ones)
std::string do_load(const std::string &model_path, const std::string &files_dir, int threads,
                    int n_ctx, bool use_mmap) {
    release_llm();

    // Make the sandbox reachable for `hdc file send` (the app's own uid owns it, so chmod is
    // allowed; without this, /data/app/.../files stays 0770 and the shell user cannot push the
    // model in). Best effort — harmless if it fails.
    if (!files_dir.empty()) {
        chmod(files_dir.c_str(), 0777);
    }

    if (!ensure_kcfg(files_dir)) {
        return "FAIL: cannot write kcfg.ini into " + files_dir;
    }

    // Resolve the model: use the requested path, else fall back to <filesDir>/model.gguf.
    std::string resolved = model_path;
    std::string note;
    if (access(resolved.c_str(), R_OK) != 0) {
        const std::string alt = files_dir + "/model.gguf";
        if (access(alt.c_str(), R_OK) == 0) {
            resolved = alt;
            note = " [fell back to sandbox path]";
        }
    }
    if (access(resolved.c_str(), R_OK) != 0) {
        return "FAIL: model not readable: " + model_path + " (and " + files_dir +
               "/model.gguf). Push it with: hdc file send <local.gguf> " + files_dir +
               "/model.gguf";
    }

    const auto t0 = std::chrono::steady_clock::now();
    llama_model_params mparams = llama_model_default_params();
    mparams.use_mmap = use_mmap;
    mparams.n_gpu_layers = 0;
    llama_model *model = llama_load_model_from_file(resolved.c_str(), mparams);
    if (!model) {
        return "FAIL: llama_load_model_from_file(" + resolved + ")";
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = (uint32_t) n_ctx;
    cparams.n_batch = 512;
    cparams.n_threads = threads;
    cparams.n_threads_batch = threads;
    // NOTE: no cparams.seed in this llama.cpp version — determinism comes from the sampler
    // below (std::mt19937 seeded with 42).
    llama_context *ctx = llama_new_context_with_model(model, cparams);
    if (!ctx) {
        llama_free_model(model);
        return "FAIL: llama_new_context_with_model";
    }
    const auto t1 = std::chrono::steady_clock::now();

    g_llm.model = model;
    g_llm.ctx = ctx;
    g_llm.model_path = resolved;
    g_llm.n_threads = threads;
    g_llm.n_ctx = n_ctx;
    g_llm.load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "OK: loaded %s%s | n_vocab=%d | n_ctx=%d | threads=%d | mmap=%s | load=%.0f ms | kcfg=%s",
                  resolved.c_str(), note.c_str(), llama_n_vocab(model), n_ctx, threads,
                  use_mmap ? "on" : "off", g_llm.load_ms, g_llm.kcfg_path.c_str());
    return buf;
}

std::string do_generate(const std::string &prompt, int n_predict, double temp, int top_k) {
    if (!g_llm.model || !g_llm.ctx) return "FAIL: call loadModel() first";

    llama_model *model = g_llm.model;
    llama_context *ctx = g_llm.ctx;
    const int n_vocab = llama_n_vocab(model);

    // Each generate() call is an independent completion: drop the previous prompt/answer from
    // the KV cache, otherwise stale positions leak into the next run.
    llama_kv_cache_clear(ctx);

    std::vector<llama_token> tokens(prompt.size() + 8);
    int n_tok = llama_tokenize(model, prompt.c_str(), (int32_t) prompt.size(), tokens.data(),
                               (int32_t) tokens.size(), true, false);
    if (n_tok < 0) {
        tokens.resize(-n_tok);
        n_tok = llama_tokenize(model, prompt.c_str(), (int32_t) prompt.size(), tokens.data(),
                               (int32_t) tokens.size(), true, false);
    }
    if (n_tok <= 0) return "FAIL: tokenize";
    tokens.resize(n_tok);

    const auto t_prompt0 = std::chrono::steady_clock::now();
    llama_batch batch = llama_batch_get_one(tokens.data(), n_tok, 0, 0);
    if (llama_decode(ctx, batch) != 0) return "FAIL: prompt decode";
    const auto t_prompt1 = std::chrono::steady_clock::now();

    std::mt19937 rng(42);
    const llama_token eos = llama_token_eos(model);
    std::string text;
    int n_past = n_tok;
    int generated = 0;

    const auto t_eval0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n_predict && n_past < g_llm.n_ctx - 1; ++i) {
        float *logits = llama_get_logits(ctx);
        if (!logits) break;
        const llama_token id =
            (temp <= 0.0) ? (llama_token) (std::max_element(logits, logits + n_vocab) - logits)
                          : sample_top_k(logits, n_vocab, top_k, (float) temp, rng);
        if (id == eos) break;
        char piece[256];
        const int np = llama_token_to_piece(model, id, piece, sizeof(piece), 0, false);
        if (np > 0) text.append(piece, (size_t) np);
        llama_batch b1 = llama_batch_get_one((llama_token *) &id, 1, n_past, 0);
        if (llama_decode(ctx, b1) != 0) break;
        ++n_past;
        ++generated;
    }
    const auto t_eval1 = std::chrono::steady_clock::now();

    const double prompt_ms = std::chrono::duration<double, std::milli>(t_prompt1 - t_prompt0).count();
    const double eval_ms = std::chrono::duration<double, std::milli>(t_eval1 - t_eval0).count();
    char timing[640];
    std::snprintf(timing, sizeof(timing),
                  "\n----\nprompt: %d tok / %.1f ms (%.2f tok/s)\neval: %d tok / %.1f ms (%.2f tok/s)\n"
                  "load: %.0f ms | threads=%d | model=%s",
                  n_tok, prompt_ms, n_tok / (prompt_ms / 1000.0), generated, eval_ms,
                  generated > 0 ? generated / (eval_ms / 1000.0) : 0.0, g_llm.load_ms, g_llm.n_threads,
                  g_llm.model_path.c_str());
    return text + timing;
}

// ---------------------------------------------------------------- async plumbing (Promise + worker thread)
struct AsyncJob {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    int op = 0;             // 0 = load, 1 = generate
    std::string s1, s2;     // load: model_path/files_dir, generate: prompt
    int threads = 4, n_ctx = 512, n_predict = 32, top_k = 40;
    double temp = 0.8;
    bool use_mmap = false;
    std::string result;
};

void execute_job(napi_env, void *data) {
    AsyncJob *j = static_cast<AsyncJob *>(data);
    if (j->op == 0) {
        j->result = do_load(j->s1, j->s2, j->threads, j->n_ctx, j->use_mmap);
    } else {
        j->result = do_generate(j->s2, j->n_predict, j->temp, j->top_k);
    }
}

void complete_job(napi_env env, napi_status status, void *data) {
    AsyncJob *j = static_cast<AsyncJob *>(data);
    napi_value result = nullptr;
    napi_create_string_utf8(env, j->result.c_str(), j->result.size(), &result);
    if (status == napi_ok) {
        napi_resolve_deferred(env, j->deferred, result);
    } else {
        napi_value msg = nullptr;
        napi_create_string_utf8(env, "native async work failed", NAPI_AUTO_LENGTH, &msg);
        napi_value err = nullptr;
        napi_create_error(env, nullptr, msg, &err);
        napi_reject_deferred(env, j->deferred, err);
    }
    napi_delete_async_work(env, j->work);
    delete j;
}

napi_value queue_job(napi_env env, AsyncJob *j, const char *name) {
    napi_value promise = nullptr;
    napi_create_promise(env, &j->deferred, &promise);
    napi_value resource = nullptr;
    napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &resource);
    napi_create_async_work(env, nullptr, resource, execute_job, complete_job, j, &j->work);
    napi_queue_async_work(env, j->work);
    return promise;
}

// ---------------------------------------------------------------- NAPI entry points
napi_value NativeVersion(napi_env env, napi_callback_info) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "libtmac_hap | %s | tfloat=%d B | kcfg embedded | static llama.cpp + T-MAC",
#if defined(__x86_64__)
                  "x86_64 (AVX2, no FMA)",
#else
                  "arm64 (NEON fp16)",
#endif
                  TMAC_TFLOAT_BYTES);
    return make_string(env, buf);
}

napi_value SelfTest(napi_env env, napi_callback_info) {
    return make_string(env, run_self_test());
}

napi_value Bench(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t steps = 100;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &steps);
    return make_string(env, run_bench(steps));
}

napi_value LoadModel(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return make_string(env, "FAIL: loadModel(modelPath, filesDir, [threads], [nCtx], [useMmap])");
    }
    const std::string model_path = get_string_arg(env, argv[0]);
    const std::string files_dir = get_string_arg(env, argv[1]);
    int32_t threads = 4, n_ctx = 512;
    bool use_mmap = false;
    if (argc >= 3) napi_get_value_int32(env, argv[2], &threads);
    if (argc >= 4) napi_get_value_int32(env, argv[3], &n_ctx);
    if (argc >= 5) napi_get_value_bool(env, argv[4], &use_mmap);
    return make_string(env, do_load(model_path, files_dir, threads, n_ctx, use_mmap));
}

napi_value LoadModelAsync(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    AsyncJob *j = new AsyncJob();
    j->op = 0;
    if (argc >= 1) j->s1 = get_string_arg(env, argv[0]);
    if (argc >= 2) j->s2 = get_string_arg(env, argv[1]);
    if (argc >= 3) napi_get_value_int32(env, argv[2], &j->threads);
    if (argc >= 4) napi_get_value_int32(env, argv[3], &j->n_ctx);
    if (argc >= 5) napi_get_value_bool(env, argv[4], &j->use_mmap);
    return queue_job(env, j, "tmac_load");
}

napi_value Generate(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return make_string(env, "FAIL: generate(prompt, [nPredict], [temp], [topK])");
    const std::string prompt = get_string_arg(env, argv[0]);
    int32_t n_predict = 32, top_k = 40;
    double temp = 0.8;
    if (argc >= 2) napi_get_value_int32(env, argv[1], &n_predict);
    if (argc >= 3) napi_get_value_double(env, argv[2], &temp);
    if (argc >= 4) napi_get_value_int32(env, argv[3], &top_k);
    return make_string(env, do_generate(prompt, n_predict, temp, top_k));
}

napi_value GenerateAsync(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    AsyncJob *j = new AsyncJob();
    j->op = 1;
    if (argc >= 1) j->s2 = get_string_arg(env, argv[0]);
    if (argc >= 2) napi_get_value_int32(env, argv[1], &j->n_predict);
    if (argc >= 3) napi_get_value_double(env, argv[2], &j->temp);
    if (argc >= 4) napi_get_value_int32(env, argv[3], &j->top_k);
    return queue_job(env, j, "tmac_generate");
}

napi_value Release(napi_env env, napi_callback_info) {
    release_llm();
    return make_string(env, "OK: model released");
}

// chmod the sandbox so the host can `hdc file send` a model into it, and report the exact
// path the UI should use. Call this right after app start.
//
// Why this is needed on HarmonyOS 7: an app may only read files inside its own sandbox
// (/data/local/tmp is blocked by SELinux), but `hdc file send` as the shell user cannot create
// files in the sandbox either. What DOES work: the app creates a 0666 placeholder file, and the
// shell user can then overwrite it. So we chmod the dir AND create model.gguf as 0666.
napi_value PrepareSandbox(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return make_string(env, "FAIL: prepareSandbox(filesDir)");
    const std::string files_dir = get_string_arg(env, argv[0]);
    if (files_dir.empty()) return make_string(env, "FAIL: empty filesDir");

    const int rc = chmod(files_dir.c_str(), 0777);

    const std::string model_path = files_dir + "/model.gguf";
    struct stat st;
    bool created = false;
    if (stat(model_path.c_str(), &st) != 0) {
        const int fd = open(model_path.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd >= 0) {
            close(fd);
            created = true;
        }
    }
    const bool model_present = (stat(model_path.c_str(), &st) == 0) && st.st_size > 0;

    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "sandbox=%s | chmod=%s | placeholder=%s | model.gguf %s (%lld B)\n"
                  "push: hdc file send <local>.gguf %s/model.gguf",
                  files_dir.c_str(), rc == 0 ? "0777 ok" : "failed", created ? "created 0666" : "exists",
                  model_present ? "present" : "empty",
                  (long long) (model_present ? st.st_size : 0), files_dir.c_str());
    return make_string(env, buf);
}

}  // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"nativeVersion", nullptr, NativeVersion, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"selfTest", nullptr, SelfTest, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"bench", nullptr, Bench, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"loadModel", nullptr, LoadModel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"loadModelAsync", nullptr, LoadModelAsync, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"generate", nullptr, Generate, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"generateAsync", nullptr, GenerateAsync, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"prepareSandbox", nullptr, PrepareSandbox, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"release", nullptr, Release, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module tmacModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "tmac_hap",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterTmacModule(void) {
    napi_module_register(&tmacModule);
}
