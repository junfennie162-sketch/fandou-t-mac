// sa_smoke_main.cpp —— 主机侧全流程自测（不开设备即可验证 SA 业务内核）
//
// 覆盖：CreateSession → LoadModel → WarmKernel(m128-k3200 LUT) → InferTokenBatch →
//       参考内核数值自检 → ReleaseSession
// 构建/运行：见同目录 run_host_smoke.sh（g++，x86_64 宿主机）
#include <cstdio>
#include <string>

#include "lut_kernel_ref.h"
#include "lut_sa.h"

static bool Chk(const char *name, tmac_sa::Status s) {
  std::printf("%-18s %s (code %d)\n", name, s == tmac_sa::Status::kOk ? "OK" : "FAIL",
              static_cast<int>(s));
  return s == tmac_sa::Status::kOk;
}

int main() {
  uint64_t sid = 0;
  if (!Chk("CreateSession", tmac_sa::CreateSession(&sid))) {
    return 1;
  }
  if (!Chk("LoadModel", tmac_sa::LoadModel(sid, "/data/lut_demo.gguf"))) {
    return 1;
  }
  if (!Chk("WarmKernel", tmac_sa::WarmKernel(sid, 4))) {
    return 1;
  }
  char buf[512] = {0};
  if (!Chk("InferTokenBatch",
           tmac_sa::InferTokenBatch(sid, "The capital of France is", buf, sizeof(buf)))) {
    return 1;
  }
  std::printf("  infer out : %s\n", buf);
  std::string detail;
  const bool ref_ok = tmac_sa::RefKernelSelfCheck(detail);
  std::printf("  ref kernel: %s\n", detail.c_str());
  Chk("ReleaseSession", tmac_sa::ReleaseSession(sid));
  std::printf(ref_ok ? "\nPASS: SA kernel end-to-end\n" : "\nFAIL: reference kernel\n");
  return ref_ok ? 0 : 2;
}
