// LUT-SA SystemAbility 绑定实现（编译条件见 lut_sa_ability.h）
// 已用 OH 平台头文件（safwk/samgr/ipc/c_utils/hilog）+ OH SDK clang 做编译级验证。
#ifdef TMAC_SA_SAMGR_BINDING

#include "lut_sa_ability.h"

#include "lut_kernel_ref.h"

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "hilog/log.h"
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
}  // namespace

void LutSystemAbility::OnStart() {
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
    dprintf(fd, "  lifecycle : OnStart=Publish -> samgr; OnStop=ReleaseSession\n");
    return 0;
}

ErrCode LutSystemAbility::NativeVersion(std::string &result) {
    result = "LUT-SA native | SA mode | tfloat=2 B | kcfg embedded | static llama.cpp + LUT kernels";
    return ERR_OK;
}

ErrCode LutSystemAbility::LoadModel(const std::string &path, int32_t threads, int32_t nCtx) {
    (void) threads;
    (void) nCtx;
    if (g_session == kInvalidSession) {
        const ::tmac_sa::Status st = ::tmac_sa::CreateSession(&g_session);
        if (st != ::tmac_sa::Status::kOk) {
            return ERR_INVALID_VALUE;
        }
    }
    const ::tmac_sa::Status st = ::tmac_sa::LoadModel(g_session, path);
    return st == ::tmac_sa::Status::kOk ? ERR_OK : ERR_INVALID_VALUE;
}

ErrCode LutSystemAbility::Generate(const std::string &prompt, int32_t nPredict, double temp,
                                   int32_t topK, std::string &result) {
    (void) nPredict;
    (void) temp;
    (void) topK;
    if (g_session == kInvalidSession) {
        return ERR_INVALID_VALUE;
    }
    char buf[8192];
    buf[0] = '\0';
    const ::tmac_sa::Status st = ::tmac_sa::InferTokenBatch(g_session, prompt.c_str(), buf, sizeof(buf));
    result = buf;
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
    result = buf;
    return ERR_OK;
}

ErrCode LutSystemAbility::SelfTest(std::string &result) {
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
    if (g_session != kInvalidSession) {
        (void) ::tmac_sa::ReleaseSession(g_session);
        g_session = kInvalidSession;
    }
    result = "ok: session released, engine memory returned to system";
    return ERR_OK;
}

}  // namespace OHOS

#endif  // TMAC_SA_SAMGR_BINDING
