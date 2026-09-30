// lut_kernel_ref.cpp —— LUT-SA 便携参考内核实现（标量，无 ISA 依赖）
//
// 语义（与 t-mac 优化内核的 LUT 思路一致，用于镜像构建与自检）：
//   分组：kG=4 个权重一组；组内 lane=0..3，码字 code ∈ [0, 2^bits)
//   LUT ：qlut[(n*(K/kG) + g)*16 + lane*4 + code] = int8(act(n, g*4+lane) * code)
//          —— 16 项/组（4 lane × 4 code），与 SessionWorkspace 的 qlut 缓冲尺寸一致
//   计算：C[m][n] = Σ_g scale[m][g] * Σ_lane qlut[g][lane*4+code(A,m,g*4+lane)]
//   存储：A 行主序按 bits 打包（LSB 优先，行字节数 = ceil(K*bits/8)）；scale/输出为 fp16 位模式
//
// 支持 bits=2（本作品 2.44 bit/权重的 2-bit 路）；bits=4 返回 -2（参考内核不覆盖，走优化内核）。
#include "lut_kernel_ref.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

constexpr int kG = 4;                      // 每组权重数（与 SessionWorkspace::kG 一致）
constexpr int kLutPerGroup = 16;           // 4 lane × 4 code
constexpr int kWmax = 4096;                // 参考内核 m 上限（防御性，非语义限制）

inline float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1u;
  const uint32_t e = (h >> 10) & 0x1Fu;
  const uint32_t f = h & 0x3FFu;
  float v;
  if (e == 0) {
    v = std::ldexp(static_cast<float>(f), -24);
  } else if (e == 0x1Fu) {
    v = f ? NAN : INFINITY;
  } else {
    v = std::ldexp(static_cast<float>(f + 1024u), static_cast<int>(e) - 25);
  }
  return s ? -v : v;
}

inline uint16_t FloatToHalf(float x) {
  if (!std::isfinite(x)) {
    return static_cast<uint16_t>((x != x) ? 0x7E00 : ((x < 0) ? 0xFC00 : 0x7C00));
  }
  const float ax = std::fabs(x);
  if (ax < 6.104e-5f) {
    return static_cast<uint16_t>(x < 0 ? 0x8000 : 0);
  }
  if (ax > 65504.0f) {
    return static_cast<uint16_t>((x < 0 ? 0xFC00 : 0x7C00));
  }
  int e = 0;
  float m = std::frexp(ax, &e);                          // ax = m * 2^e, m ∈ [0.5,1)
  uint32_t bits = static_cast<uint32_t>((e + 14) << 10); // 指数域
  bits |= static_cast<uint32_t>((m * 2.0f - 1.0f) * 1024.0f + 0.5f);
  return static_cast<uint16_t>(bits | (x < 0 ? 0x8000u : 0u));
}

inline int CodeAt(const uint8_t *row, int k_index, int bits) {
  const int bitpos = k_index * bits;
  const uint8_t byte = row[bitpos >> 3];
  const int shift = bitpos & 7;
  return (byte >> shift) & ((1 << bits) - 1);
}

inline int8_t ClampI8(float v) {
  if (v > 127.0f) return 127;
  if (v < -128.0f) return -128;
  return static_cast<int8_t>(v >= 0 ? v + 0.5f : v - 0.5f);
}

}  // namespace

int preprocessor_int8(int m, int k, int n, int bits, const void *B, void *lut_scales,
                      void *lut_biases, void *qlut) {
  (void)m;
  (void)lut_biases;   // 参考内核不引入每组偏置项（优化内核另有语义）
  (void)lut_scales;   // 不写：其长度由调用方 act_group 决定，缩放由 GEMM 侧 scales 承担
  if (!B || !qlut || n <= 0 || k <= 0 || (k % kG) != 0) {
    return -1;
  }
  if (bits != 2) {
    return -2;  // 参考内核只覆盖 2-bit 路
  }
  const uint16_t *act = static_cast<const uint16_t *>(B);   // [n][k] fp16 位模式
  int8_t *q = static_cast<int8_t *>(qlut);
  const int groups = k / kG;
  for (int ni = 0; ni < n; ++ni) {
    for (int g = 0; g < groups; ++g) {
      float group_sum = 0.0f;
      for (int lane = 0; lane < kG; ++lane) {
        const float a = HalfToFloat(act[ni * k + g * kG + lane]);
        group_sum += a;
        for (int code = 0; code < 4; ++code) {
          q[(ni * groups + g) * kLutPerGroup + lane * 4 + code] = ClampI8(a * static_cast<float>(code));
        }
      }
    }
  }
  return 0;
}

int qgemm_lut_int8(int m, int k, int n, int bits, const void *A, const void *qlut,
                   const void *scales, const void *lut_scales, const void *lut_biases, void *C) {
  (void)lut_scales;
  (void)lut_biases;
  if (!A || !qlut || !scales || !C || m <= 0 || m > kWmax || n <= 0 || k <= 0 || (k % kG) != 0) {
    return -1;
  }
  if (bits != 2) {
    return -2;
  }
  const int groups = k / kG;
  const size_t row_bytes = (static_cast<size_t>(k) * bits + 7) / 8;
  const uint8_t *w = static_cast<const uint8_t *>(A);
  const int8_t *q = static_cast<const int8_t *>(qlut);
  const uint16_t *sc = static_cast<const uint16_t *>(scales);   // [m][k/kG] fp16
  uint16_t *out = static_cast<uint16_t *>(C);

  for (int mi = 0; mi < m; ++mi) {
    const uint8_t *row = w + static_cast<size_t>(mi) * row_bytes;
    for (int ni = 0; ni < n; ++ni) {
      float acc = 0.0f;
      for (int g = 0; g < groups; ++g) {
        int32_t partial = 0;
        for (int lane = 0; lane < kG; ++lane) {
          const int code = CodeAt(row, g * kG + lane, bits);
          partial += q[(ni * groups + g) * kLutPerGroup + lane * 4 + code];
        }
        acc += HalfToFloat(sc[static_cast<size_t>(mi) * groups + g]) * static_cast<float>(partial);
      }
      out[static_cast<size_t>(mi) * n + ni] = FloatToHalf(acc);
    }
  }
  return 0;
}

namespace tmac_sa {

bool RefKernelSelfCheck(std::string &detail) {
  // 用例：k=8（2 组）、bits=2、所有码字=1、激活=1.0、缩放=1.0
  // 期望：C = Σ_k code*act = 8 × 1 × 1 = 8.0
  constexpr int kM = 4;
  constexpr int kK = 8;
  constexpr int kN = 1;
  constexpr int kBits = 2;
  const size_t row_bytes = (kK * kBits + 7) / 8;   // = 2 字节
  uint8_t A[kM * row_bytes];
  std::memset(A, 0, sizeof(A));
  for (int mi = 0; mi < kM; ++mi) {                // 每个码字写 1
    for (int kk = 0; kk < kK; ++kk) {
      A[mi * row_bytes + (kk * kBits) / 8] |= static_cast<uint8_t>(1u << ((kk * kBits) % 8));
    }
  }
  uint16_t act[kK];
  for (int kk = 0; kk < kK; ++kk) {
    act[kk] = FloatToHalf(1.0f);
  }
  int8_t qlut[kN * (kK / kG) * kLutPerGroup];
  std::memset(qlut, 0, sizeof(qlut));
  uint16_t lut_scales[kN * (kK / kG)] = {};
  uint16_t lut_biases[kN * (kK / kG)] = {};
  uint16_t scales[kM * (kK / kG)];
  for (auto &s : scales) {
    s = FloatToHalf(1.0f);
  }
  uint16_t out[kM * kN] = {};

  if (preprocessor_int8(kM, kK, kN, kBits, act, lut_scales, lut_biases, qlut) != 0) {
    detail = "FAIL: preprocessor_int8 (ref)";
    return false;
  }
  if (qgemm_lut_int8(kM, kK, kN, kBits, A, qlut, scales, lut_scales, lut_biases, out) != 0) {
    detail = "FAIL: qgemm_lut_int8 (ref)";
    return false;
  }
  const float got = HalfToFloat(out[0]);
  const float expect = static_cast<float>(kK);   // 8.0
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "PASS: ref LUT kernel numeric check (k=%d bits=%d got=%.3f expect=%.1f)", kK, kBits,
                static_cast<double>(got), static_cast<double>(expect));
  detail = buf;
  return std::fabs(got - expect) < 1e-3f;
}

}  // namespace tmac_sa
