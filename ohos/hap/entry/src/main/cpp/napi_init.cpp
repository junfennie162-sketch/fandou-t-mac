// LUT-SA NAPI bridge — exposes the T-MAC LUT kernel to ArkTS.
//
// Kernel shape follows the arch selected in CMakeLists.txt:
//   x86_64 -> deploy/tuned         qgemm_lut_int8(m=256, k=4096, n=1, b=4)   (AVX2)
//   arm64  -> aarch64-hf-bitnet-3b qgemm_lut_int8(m=128, k=3200, n=1, b=2)   (NEON)
//
// The buffers are kept as raw bytes on purpose: fp16 tensors are touched
// only through the kernel, this bridge just allocates and checks zero-ness.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "napi/native_api.h"
#include "t-mac/kernels.h"

namespace {

#if defined(__x86_64__)
constexpr int kMBits = 256;
constexpr int kK = 4096;
constexpr int kBits = 4;
#else
constexpr int kMBits = 128;
constexpr int kK = 3200;
constexpr int kBits = 2;
#endif
constexpr int kN = 1;
constexpr int kG = 4;
constexpr int kActGroup = 32;
constexpr int kOutM = kMBits / kBits;
constexpr size_t kScalesBytes = 524288;  // deploy/tuned/kcfg.ini scales_size

void* alloc64(size_t nbytes) {
  void* p = nullptr;
  if (posix_memalign(&p, 64, nbytes) != 0 || !p) {
    return nullptr;
  }
  std::memset(p, 0, nbytes);
  return p;
}

struct Buffers {
  void* A = nullptr;
  void* scales = nullptr;
  void* qlut = nullptr;
  void* lut_scales = nullptr;
  void* lut_biases = nullptr;
  void* C = nullptr;

  bool ok() const {
    return A && scales && qlut && lut_scales && lut_biases && C;
  }

  ~Buffers() {
    std::free(A);
    std::free(scales);
    std::free(qlut);
    std::free(lut_scales);
    std::free(lut_biases);
    std::free(C);
  }
};

Buffers make_buffers() {
  Buffers b;
  b.A = alloc64(static_cast<size_t>(kMBits) * kK);
  b.scales = alloc64(kScalesBytes);
  b.qlut = alloc64(static_cast<size_t>(kN) * (kK / kG) * (1 << kG));
  b.lut_scales = alloc64(static_cast<size_t>(kN) * (kK / kActGroup) * sizeof(uint16_t));
  b.lut_biases = alloc64(static_cast<size_t>(kN) * (kK / kActGroup) * sizeof(uint16_t));
  b.C = alloc64(static_cast<size_t>(kN) * kOutM * sizeof(uint16_t));
  return b;
}

bool all_zero(const void* p, size_t nbytes) {
  const uint8_t* q = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < nbytes; ++i) {
    if (q[i] != 0) {
      return false;
    }
  }
  return true;
}

int run_kernel_once(Buffers& b) {
  // Non-zero C beforehand — the kernel must clear then write.
  std::memset(b.C, 0x5a, static_cast<size_t>(kN) * kOutM * sizeof(uint16_t));
  return qgemm_lut_int8(kMBits, kK, kN, kBits, b.A, b.qlut, b.scales, b.lut_scales, b.lut_biases,
                        b.C);
}

std::string run_self_test() {
  Buffers b = make_buffers();
  if (!b.ok()) {
    return "FAIL: buffer allocation";
  }
  char local[256];
  std::snprintf(local, sizeof(local), "shape m=%d k=%d n=%d b=%d | ", kMBits, kK, kN, kBits);
  std::string out = local;

  const int ret = run_kernel_once(b);
  if (ret != 0) {
    std::snprintf(local, sizeof(local), "FAIL: qgemm_lut_int8 returned %d", ret);
    return out + local;
  }
  const size_t c_bytes = static_cast<size_t>(kN) * kOutM * sizeof(uint16_t);
  if (!all_zero(b.C, c_bytes)) {
    return out + "FAIL: zero-input produced non-zero output";
  }
  return out + "PASS: kernel ran, zero-in => zero-out";
}

std::string run_bench(int steps) {
  if (steps <= 0 || steps > 10000) {
    return "FAIL: steps must be in [1, 10000]";
  }
  Buffers b = make_buffers();
  if (!b.ok()) {
    return "FAIL: buffer allocation";
  }

  // Warmup, then timed loop.
  if (run_kernel_once(b) != 0) {
    return "FAIL: qgemm_lut_int8 error during warmup";
  }
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < steps; ++i) {
    if (run_kernel_once(b) != 0) {
      return "FAIL: qgemm_lut_int8 error during bench";
    }
  }
  const auto t1 = std::chrono::steady_clock::now();

  const double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  char local[256];
  std::snprintf(local, sizeof(local),
                "shape m=%d k=%d n=%d b=%d | steps=%d | total=%.3f ms | avg=%.4f ms", kMBits, kK,
                kN, kBits, steps, total_ms, total_ms / steps);
  return local;
}

napi_value make_string(napi_env env, const std::string& s) {
  napi_value v = nullptr;
  napi_create_string_utf8(env, s.c_str(), s.length(), &v);
  return v;
}

napi_value SelfTest(napi_env env, napi_callback_info /*info*/) {
  return make_string(env, run_self_test());
}

napi_value Bench(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t steps = 100;
  if (argc >= 1) {
    napi_get_value_int32(env, argv[0], &steps);
  }
  return make_string(env, run_bench(steps));
}

}  // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor desc[] = {
      {"selfTest", nullptr, SelfTest, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"bench", nullptr, Bench, nullptr, nullptr, nullptr, napi_default, nullptr},
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
