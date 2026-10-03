// t-mac/tmac_gemm_wrapper.h —— 引擎接口头（现指向真 · 调优内核）
//
// 说明：本文件与 t-mac 引擎的同名头文件**路径一致**，用于在没有引擎源码/预编译库的
// 目标（如 QEMU x86_64 标准系统镜像）上编译 LUT-SA 业务层；只提供业务层实际用到的
// 符号（对齐常量 + 包装器壳），计算由 lut_kernel_ref.cpp 的参考内核完成。
//
// aarch64 生产路径：把 include 顺序切到引擎的 t-mac/ 目录（含真实 TMACGeMMWrapper），
// 参考内核不参与链接 —— 见 ohos/sa/QEMU-DEPLOY.md。
#pragma once

#include "t-mac/kernels.h"     // ★ 真 · 调优内核分派器（deploy/tuned/<目标>/kernels.h）

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#else
// 便携路径：半精度只作 2 字节类型使用（参考内核按 float 计算后转半精度存储）
using float16_t = uint16_t;
#endif

namespace TMAC {

constexpr size_t kAllocAlignment = 64;

// 引擎包装器的“壳”：QEMU 参考内核路径下只需记录形状，计算走 qgemm_lut_int8。
template <typename T, int G>
class TMACGeMMWrapper {
 public:
  TMACGeMMWrapper(int n_threads, int act_group, const std::string &kcfg_path,
                  const std::string &fallback = "")
      : n_threads_(n_threads > 0 ? n_threads : 1),
        act_group_(act_group > 0 ? act_group : 32),
        kcfg_path_(kcfg_path.empty() ? fallback : kcfg_path) {}

  void set_workspace(int max_k, int max_n) {
    max_k_ = max_k;
    max_n_ = max_n;
  }

  int n_threads() const { return n_threads_; }
  int act_group() const { return act_group_; }
  int max_k() const { return max_k_; }
  int max_n() const { return max_n_; }
  const std::string &kcfg_path() const { return kcfg_path_; }

 private:
  int n_threads_ = 1;
  int act_group_ = 32;
  int max_k_ = 0;
  int max_n_ = 0;
  std::string kcfg_path_;
};

}  // namespace TMAC
