// gguf_admission.cc —— 模型准入检查（系统能力不该被一份不匹配的模型打死）
//
// 背景（STA-3 实测）：bitnet-3b-tmac.gguf 的量化张量是 t-mac 的 2bit LUT 类型（ggml type 37），
// 而 t-mac 的 ggml 补丁（ggml-tmac.cpp）在**加载期**就要给每个这类张量查"形状参数表"(kcfg)：
// 查不到就 LOG(FATAL)（原实现直接 abort()）。实测会把 SA 进程打掉 → 客户端只看到
// ERR_DEAD_OBJECT(29189)，看不出为什么。
//
// 所以：**在把文件交给引擎之前**，自己按 gguf 格式读出张量清单，把形状与 kcfg 里的形状对一遍，
// 不匹配就带原因拒绝（并列出缺失的形状），引擎根本不碰这个文件。
//
// 逻辑刻意与 ggml-tmac.cpp 保持一致：token_embd.weight / output.weight 只是警告不致命。
#include "engine_shim.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kGgufMagic = 0x46554747u;  // "GGUF"

// ---- 小端读取（gguf 一律小端）----
struct Reader {
  FILE* f = nullptr;
  bool ok = true;

  bool Read(void* dst, size_t n) {
    if (!ok || std::fread(dst, 1, n, f) != n) {
      ok = false;
      return false;
    }
    return true;
  }
  uint32_t U32() {
    uint32_t v = 0;
    Read(&v, 4);
    return v;
  }
  uint64_t U64() {
    uint64_t v = 0;
    Read(&v, 8);
    return v;
  }
  std::string Str() {
    const uint64_t n = U64();
    if (!ok || n > (1u << 24)) {  // 防御：字段长度异常就判失败
      ok = false;
      return {};
    }
    std::string s(static_cast<size_t>(n), '\0');
    if (n > 0) {
      Read(&s[0], static_cast<size_t>(n));
    }
    return s;
  }
  void Skip(uint64_t n) {
    if (!ok) {
      return;
    }
    if (std::fseek(f, static_cast<long>(n), SEEK_CUR) != 0) {
      ok = false;
    }
  }
};

// gguf 的 KV 值类型 → 定长字节数（类型 8=STRING、9=ARRAY 特殊处理）
size_t FixedValueSize(uint32_t t) {
  switch (t) {
    case 0: case 1: case 7: return 1;   // U8 I8 BOOL
    case 2: case 3: return 2;           // U16 I16
    case 4: case 5: case 6: return 4;   // U32 I32 F32
    case 10: case 11: case 12: return 8; // U64 I64 F64
    default: return 0;
  }
}

void SkipValue(Reader& r, uint32_t type) {
  if (type == 8) {              // STRING
    (void) r.Str();
    return;
  }
  if (type == 9) {              // ARRAY: 元素类型 + 个数 + 逐个
    const uint32_t et = r.U32();
    const uint64_t n = r.U64();
    const size_t fs = FixedValueSize(et);
    if (fs != 0) {
      r.Skip(fs * n);
    } else {
      for (uint64_t i = 0; i < n && r.ok; ++i) {
        SkipValue(r, et);
      }
    }
    return;
  }
  r.Skip(FixedValueSize(type));
}

// 从 kcfg.ini 里读已知形状：[qgemm_lut_t1_int8_m<M>_k<K>_n1_b2] 段落
std::set<std::pair<uint64_t, uint64_t>> LoadKnownShapes(const char* kcfg_path) {
  std::set<std::pair<uint64_t, uint64_t>> out;
  FILE* f = std::fopen(kcfg_path, "r");
  if (f == nullptr) {
    return out;
  }
  char line[256];
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    unsigned long long m = 0, k = 0;
    if (std::sscanf(line, "[qgemm_lut_t1_int8_m%llu_k%llu_n1_b2]", &m, &k) == 2) {
      out.insert({m, k});
    }
  }
  std::fclose(f);
  return out;
}

bool IsWarnOnlyTensor(const std::string& name) {
  return name == "token_embd.weight" || name == "output.weight";
}

}  // namespace

extern "C" int lut_engine_admission_check(const char* model_path, const char* kcfg_path, char* err,
                                          size_t err_cap) {
  const auto known = LoadKnownShapes(kcfg_path);
  if (known.empty()) {
    return 0;  // 没有形状表就跳过准入（别把"读不到 kcfg"变成拒绝加载）
  }

  FILE* f = std::fopen(model_path, "rb");
  if (f == nullptr) {
    return 0;  // 打不开交给引擎去报错
  }
  Reader r{f};
  const uint32_t magic = r.U32();
  const uint32_t version = r.U32();
  const uint64_t n_tensors = r.U64();
  const uint64_t n_kv = r.U64();
  if (!r.ok || magic != kGgufMagic || version < 2 || n_tensors > (1u << 20)) {
    std::fclose(f);
    return 0;  // 不是 gguf v2/v3 → 不拦，交给引擎
  }

  for (uint64_t i = 0; i < n_kv && r.ok; ++i) {
    (void) r.Str();                 // key
    SkipValue(r, r.U32());          // value
  }

  uint64_t n_tmac = 0;
  std::vector<std::string> missing;
  for (uint64_t i = 0; i < n_tensors && r.ok; ++i) {
    const std::string name = r.Str();
    const uint32_t n_dims = r.U32();
    if (n_dims == 0 || n_dims > 4) {
      r.ok = false;
      break;
    }
    uint64_t dims[4] = {0, 0, 0, 0};
    for (uint32_t d = 0; d < n_dims; ++d) {
      dims[d] = r.U64();
    }
    const uint32_t type = r.U32();
    (void) r.U64();                 // offset

    // 只关心 t-mac 的 LUT 类型。类型号由引擎侧提供（ggml_tmac_get_type_bits），
    // 这里用"已知的 t-mac 专家类型号"判断：本 fork 里 2bit LUT = 37
    if (type != 37 || n_dims < 2) {
      continue;
    }
    ++n_tmac;
    const uint64_t k = dims[0];
    const uint64_t m = dims[1];
    if (known.count({m, k}) != 0) {
      continue;
    }
    if (IsWarnOnlyTensor(name)) {
      continue;  // 与 ggml-tmac 一致：词表/输出层只警告
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s(m=%llu,k=%llu)", name.c_str(),
                  static_cast<unsigned long long>(m), static_cast<unsigned long long>(k));
    if (missing.size() < 6) {
      missing.push_back(buf);
    }
  }
  std::fclose(f);

  if (missing.empty()) {
    return 0;
  }
  std::string list;
  for (size_t i = 0; i < missing.size(); ++i) {
    if (i != 0) {
      list += ", ";
    }
    list += missing[i];
  }
  char shapes[256];
  shapes[0] = '\0';
  size_t used = 0;
  for (const auto& s : known) {
    char one[48];
    const int n = std::snprintf(one, sizeof(one), "m%llu_k%llu ", static_cast<unsigned long long>(s.first),
                                static_cast<unsigned long long>(s.second));
    if (n <= 0 || used + static_cast<size_t>(n) + 1 >= sizeof(shapes)) {
      break;
    }
    std::memcpy(shapes + used, one, static_cast<size_t>(n));
    used += static_cast<size_t>(n);
    shapes[used] = '\0';
  }
  std::snprintf(err, err_cap,
                "admission rejected: %llu t-mac tensors, no LUT kernel for [%s] (kcfg has: %s)",
                static_cast<unsigned long long>(n_tmac), list.c_str(), shapes);
  return 1;
}
