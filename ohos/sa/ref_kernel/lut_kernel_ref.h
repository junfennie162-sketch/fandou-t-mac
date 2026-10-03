// lut_kernel_ref.h —— LUT-SA 便携参考内核（与 t-mac 优化内核同接口）
//
// 为什么有这份：t-mac 优化内核（NEON/AVX2）以预编译库形式随 HAP 交付，无法随 OH 系统组件
// 编进 QEMU 镜像；本文件给出**同签名的标量参考实现**，语义一致、可数值自检，
// 供 x86_64 / 任意架构的镜像构建与单元自测使用。
#pragma once

#include <cstddef>
#include <string>

// 与 t-mac 引擎 kernels.h 同名的自由函数（全局命名空间，签名保持一致）
//
// ref_preprocessor_int8: 依据 int8 激活值 B 预计算 LUT 表（qlut）与每组缩放/偏置
//   m = 逻辑行数 × bits（位切分后的行数），k = K，n = N
//   B: float16[N*K]（或 U16 半精度位模式）
//   lut_scales/lut_biases: 每组（act_group 个权重）一组 fp16 缩放/偏置
//   qlut: [n][k/kG][2^kG] 的 int8 乘积表
// ref_qgemm_lut_int8: 用 LUT 做 2/4bit 权重的反量化 GEMM
//   A: 打包权重码（每行 ceil(k*bits/8) 字节，LSB 优先）
//   scales: 每组（kG=4）一个 fp16 缩放
//   C: 输出（按 fp16 位模式存储，m*n 个）
int ref_preprocessor_int8(int m, int k, int n, int bits, const void *B, void *lut_scales,
                      void *lut_biases, void *qlut);
int ref_qgemm_lut_int8(int m, int k, int n, int bits, const void *A, const void *qlut,
                   const void *scales, const void *lut_scales, const void *lut_biases, void *C);

namespace tmac_sa {

// 参考内核数值自检：构造已知码字 → 期望值比对（同时验证 LUT 路径与缩放）。
// 返回 true 表示通过；detail 里带一行可打印结论。
bool RefKernelSelfCheck(std::string &detail);

}  // namespace tmac_sa
