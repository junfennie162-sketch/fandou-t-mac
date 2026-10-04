// LUT-SA SystemAbility 绑定实现（编译条件见 lut_sa_ability.h）
// 已用 OH 平台头文件（safwk/samgr/ipc/c_utils/hilog）+ OH SDK clang 做编译级验证。
#ifdef TMAC_SA_SAMGR_BINDING

#include "lut_sa_ability.h"

#include "lut_kernel_ref.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "hilog/log.h"
#include "ipc_skeleton.h"   // S5-1：取调用方 uid/tokenId
#include "../lut_sa.h"   // 业务内核（全局命名空间 tmac_sa::）

namespace OHOS {

REGISTER_SYSTEM_ABILITY_BY_ID(LutSystemAbility, 6901, true);

namespace {
constexpr int32_t kLutSaId = 6901;
constexpr uint64_t kInvalidSession = 0;
uint64_t g_session = kInvalidSession;   // 简化：单会话；多会话表见 session_workspace
// 最近一次 SelfTest 的详情：stub 在 ErrCode!=0 时不回传结果串，
// 而本 SA 的 hilog 在 guest 里抓不到 → 借 GetMetrics（能通）把详情带出去
std::string g_lastSelfTest = "(selftest not run yet)";

// ---------------------------------------------------------------------------
// S5-1 调用方身份与准入（谁能调、调到哪一档）
//
// 档位设计（先把"地基"立住，后面再细化）：
//   Tier-A 系统/特权 uid（root 0 / system 1000 / shell 2000 / OH 内部服务 uid < 10000）
//          → 全部方法可用（只读 + 推理）
//   Tier-B 其余（典型是第三方应用 uid ≥ 10000）
//          → 默认只允许只读方法（NativeVersion / GetMetrics）；LoadModel / Generate /
//            SelfTest / Release 这些"要占内存、要算力"的方法返回 201 ERR_PERMISSION_DENIED
//   ▸ 运维/实验开关：/data/lut_sa/allow_uids.txt 存在且非空 → **白名单模式**，只认表里的 uid
//     （这样在没有第二个 uid 可用的 QEMU 环境里也能验证"拒绝"这条路：把 root 排除即可）
//
// 真实部署的下一步（写进 PLAN）：注册一个 ohos.permission.LUT_SA_INFER（system_grant）给系统应用，
// Tier-B 走"应用申请权限 + 配额"的路子；这里先用 uid + 白名单把门立起来，语义与拒绝码先固定下来。
constexpr int32_t kErrPermissionDenied = 201;  // OH: ERR_PERMISSION_DENIED
const char* kAllowUidsFile = "/data/lut_sa/allow_uids.txt";
std::string g_lastCaller = "(none)";
int g_allow_count = -1;   // -1 = 默认档位模式，>=0 = 白名单模式（表内条数）

bool IsPrivilegedUid(uint32_t uid) {
    return uid == 0 || uid == 1000 || uid == 2000 || uid < 10000;
}

// 读白名单；返回 true 表示"白名单模式生效"
bool LoadAllowList(std::vector<uint32_t>& out) {
    FILE* f = fopen(kAllowUidsFile, "r");
    if (f == nullptr) {
        return false;
    }
    char line[64];
    while (fgets(line, sizeof(line), f) != nullptr) {
        char* end = nullptr;
        const unsigned long v = strtoul(line, &end, 10);
        if (end != line) {
            out.push_back(static_cast<uint32_t>(v));
        }
    }
    fclose(f);
    return !out.empty();
}

// 记录调用方 + 判定是否允许"推理类"方法
bool InferAllowed(const char* method) {
    const uint32_t uid = static_cast<uint32_t>(IPCSkeleton::GetCallingUid());
    const uint64_t token = static_cast<uint64_t>(IPCSkeleton::GetCallingTokenID());
    char buf[128];
    snprintf(buf, sizeof(buf), "uid=%u token=%llu last_method=%s", uid,
             static_cast<unsigned long long>(token), method);
    g_lastCaller = buf;

    std::vector<uint32_t> allow;
    if (LoadAllowList(allow)) {
        g_allow_count = static_cast<int>(allow.size());
        for (uint32_t u : allow) {
            if (u == uid) {
                return true;
            }
        }
        HILOG_ERROR(LOG_CORE, "[LutSa] %{public}s denied: %{public}s (whitelist mode, %{public}d uids)",
                    method, g_lastCaller.c_str(), g_allow_count);
        return false;
    }
    g_allow_count = -1;
    if (!IsPrivilegedUid(uid)) {
        HILOG_ERROR(LOG_CORE, "[LutSa] %{public}s denied: %{public}s (default tiers)", method,
                    g_lastCaller.c_str());
        return false;
    }
    return true;
}

std::string PolicyDesc() {
    char b[96];
    if (g_allow_count >= 0) {
        snprintf(b, sizeof(b), "whitelist(%d uids in %s)", g_allow_count, kAllowUidsFile);
    } else {
        snprintf(b, sizeof(b), "default-tiers(uid 0/1000/2000/<10000 allowed)");
    }
    return std::string(b);
}
}  // namespace

void LutSystemAbility::OnStart() {
    // 诊断通道（STA-3）：引擎的 GGML_ASSERT / LOG(FATAL) 都往 stderr 打，而 SA 进程的
    // stderr 在 guest 里抓不到（hilog 里从来没有 [LutSa] 行）→ 落文件，由取证脚本/宿主机读走。
    // 目录用 /data/lut_sa（注入镜像时建成 0777）：/data/local/tmp 对 system uid 只有 --x，
    // SA 在里面根本建不了文件（踩过：文件压根没出现）。
    (void) mkdir("/data/lut_sa", 0777);
    (void) freopen("/data/lut_sa/rt_stderr.txt", "a", stderr);
    (void) freopen("/data/lut_sa/rt_stdout.txt", "a", stdout);
    HILOG_INFO(LOG_CORE, "[LutSa] OnStart (SA_ID=%{public}d)", kLutSaId);
    const bool ok = Publish(this);   // 登记到 samgr：全系统可 LoadSystemAbility(6901)
    HILOG_INFO(LOG_CORE, "[LutSa] Publish: %{public}s", ok ? "ok" : "fail");
    if (!ok) {
        return;
    }
    const ::tmac_sa::Status st = ::tmac_sa::CreateSession(&g_session);
    HILOG_INFO(LOG_CORE, "[LutSa] CreateSession: %{public}d", static_cast<int>(st));
}

void LutSystemAbility::OnStop() {
    if (g_session != kInvalidSession) {
        (void) ::tmac_sa::ReleaseSession(g_session);
        g_session = kInvalidSession;
    }
    HILOG_INFO(LOG_CORE, "[LutSa] OnStop, session released");
}

int32_t LutSystemAbility::OnSvcCmd(int32_t fd, const std::vector<std::u16string> &args) {
    dprintf(fd, "LutSystemAbility (SA_ID=%d) -- LUT-SA on-device low-bit LLM inference service\n", kLutSaId);
    dprintf(fd, "  mode      : %s\n", args.empty() ? "summary" : "detail");
    dprintf(fd, "  session   : %llu\n", static_cast<unsigned long long>(g_session));
    dprintf(fd, "  kernel    : m128-k3200 LUT (kcfg embedded), 2.44 bit/weight\n");
    dprintf(fd, "  %s\n", ::tmac_sa::EngineInfo().c_str());
    dprintf(fd, "  lifecycle : OnStart=Publish -> samgr; OnStop=ReleaseSession\n");
    return 0;
}

ErrCode LutSystemAbility::NativeVersion(std::string &result) {
    result = "LUT-SA native | SA mode | tfloat=2 B | kcfg embedded | static llama.cpp + LUT kernels";
    return ERR_OK;
}

ErrCode LutSystemAbility::LoadModel(const std::string &path, int32_t threads, int32_t nCtx) {
    if (!InferAllowed("LoadModel")) {   // S5-1 准入
        return kErrPermissionDenied;
    }
    if (g_session == kInvalidSession) {
        const ::tmac_sa::Status st = ::tmac_sa::CreateSession(&g_session);
        if (st != ::tmac_sa::Status::kOk) {
            return ERR_INVALID_VALUE;
        }
    }
    // STA-3：threads / nCtx 不再吞掉，直接进引擎参数（<=0 时用 SessionConfig 的默认值）
    const auto t0 = std::chrono::steady_clock::now();
    const ::tmac_sa::Status st = ::tmac_sa::LoadModel(g_session, path, threads, nCtx);
    const auto t1 = std::chrono::steady_clock::now();
    if (st != ::tmac_sa::Status::kOk) {
        HILOG_ERROR(LOG_CORE, "[LutSa] LoadModel fail(%{public}d): %{public}s",
                    static_cast<int>(st), path.c_str());
        return ERR_INVALID_VALUE;
    }
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    HILOG_INFO(LOG_CORE, "[LutSa] LoadModel ok in %{public}.1f ms: %{public}s", ms,
               ::tmac_sa::EngineInfo().c_str());
    return ERR_OK;
}

ErrCode LutSystemAbility::Generate(const std::string &prompt, int32_t nPredict, double temp,
                                   int32_t topK, std::string &result) {
    if (!InferAllowed("Generate")) {    // S5-1 准入
        return kErrPermissionDenied;
    }
    if (g_session == kInvalidSession) {
        return ERR_INVALID_VALUE;
    }
    // 生成文本按 nPredict 估容量（每 token 最多几个字节，中文更宽），上限 256 KB：
    // 原来固定 8 KB，长回复会被 snprintf 静默截断
    const int want = nPredict > 0 ? nPredict : 24;
    const size_t cap = std::min<size_t>(256 * 1024, 1024 + 64 * static_cast<size_t>(want));
    std::vector<char> buf(cap, 0);
    const auto t0 = std::chrono::steady_clock::now();
    const ::tmac_sa::Status st =
        ::tmac_sa::InferTokenBatch(g_session, prompt.c_str(), buf.data(), buf.size(), nPredict,
                                   temp, topK);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    HILOG_INFO(LOG_CORE, "[LutSa] Generate %{public}.1f ms, %{public}zu bytes, status=%{public}d",
               ms, std::strlen(buf.data()), static_cast<int>(st));
    result.assign(buf.data());
    return st == ::tmac_sa::Status::kOk ? ERR_OK : ERR_INVALID_VALUE;
}

ErrCode LutSystemAbility::GetMetrics(std::string &result) {
    long peak_kb = 0;
    if (FILE *st = fopen("/proc/self/status", "r")) {
        char line[256];
        while (fgets(line, sizeof(line), st)) {
            if (strncmp(line, "VmHWM:", 6) == 0) {
                peak_kb = atol(line + 6);
                break;
            }
        }
        fclose(st);
    }
    char buf[192];
    snprintf(buf, sizeof(buf), "peak_rss=%.1f MB, session=%llu, sa_id=%d, selftest: %s",
             static_cast<double>(peak_kb) / 1024.0, static_cast<unsigned long long>(g_session), kLutSaId,
             g_lastSelfTest.c_str());
    // STA-3：引擎状态（含 llama.cpp 日志尾巴）搭这条能通的通道带出去
    // S5-1：顺带把"最近一次调用方身份 + 当前准入策略"也带出去（只读方法，无需准入）
    result = std::string(buf) + " | caller: " + g_lastCaller + " | policy: " + PolicyDesc() +
             " | " + ::tmac_sa::EngineInfo();
    return ERR_OK;
}

ErrCode LutSystemAbility::SelfTest(std::string &result) {
    if (!InferAllowed("SelfTest")) {    // S5-1 准入
        return kErrPermissionDenied;
    }
    if (g_session == kInvalidSession) {
        const ::tmac_sa::Status st = ::tmac_sa::CreateSession(&g_session);
        if (st != ::tmac_sa::Status::kOk) {
            result = "FAIL: CreateSession";
            return ERR_INVALID_VALUE;
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    const ::tmac_sa::Status st = ::tmac_sa::WarmKernel(g_session, 4);
    const auto t1 = std::chrono::steady_clock::now();
    const double warm_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    std::string ref_detail;
    const bool ref_ok = ::tmac_sa::RefKernelSelfCheck(ref_detail);
    const bool warm_ok = (st == ::tmac_sa::Status::kOk);
    char warm_msg[160];
    snprintf(warm_msg, sizeof(warm_msg),
             "%s (m128-k3200 b2, 调优内核, %.1f us)",
             warm_ok ? "PASS: LUT kernel warm-up" : "FAIL: WarmKernel", warm_us);
    result = std::string(warm_msg) + " | " + ref_detail;
    // stub 在 ErrCode!=0 时不会把结果串回传，客户端那边只能看到错误码 →
    // 这里同步写一份 hilog，便于串口/hilog 取证通道看到到底是哪一项没过
    g_lastSelfTest = result;
    HILOG_INFO(LOG_CORE, "[LutSa] SelfTest: %{public}s", result.c_str());
    return (warm_ok && ref_ok) ? ERR_OK : ERR_INVALID_VALUE;
}

ErrCode LutSystemAbility::Release(std::string &result) {
    if (!InferAllowed("Release")) {     // S5-1 准入
        return kErrPermissionDenied;
    }
    if (g_session != kInvalidSession) {
        (void) ::tmac_sa::ReleaseSession(g_session);
        g_session = kInvalidSession;
    }
    result = "ok: session released, engine memory returned to system";
    return ERR_OK;
}

}  // namespace OHOS

#endif  // TMAC_SA_SAMGR_BINDING
