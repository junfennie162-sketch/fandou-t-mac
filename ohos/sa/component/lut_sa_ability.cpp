// LUT-SA SystemAbility 绑定实现（编译条件见 lut_sa_ability.h）
#ifdef TMAC_SA_SAMGR_BINDING

#include "lut_sa_ability.h"

#include <string>
#include <vector>

#include "hilog/log.h"
#include "../lut_sa.h"   // 业务内核：CreateSession/PrepareWorkspace/WarmKernel/...

namespace tmac_sa {

// SA_ID = 6901（与 sa_profile/lut_sa.json 一致）；run-on-create = true（应用 LoadSystemAbility 时拉起）
REGISTER_SYSTEM_ABILITY_BY_ID(LutSystemAbility, 6901, true);

namespace {
constexpr const char *kLogTag = "LutSa";
constexpr int32_t kLutSaId = 6901;

uint64_t g_session = 0;   // 简化：单会话；多会话表见 session_workspace
}  // namespace

void LutSystemAbility::OnStart() {
    HILOG_INFO(LOG_CORE, "[%{public}s] OnStart (SA_ID=%{public}d)", kLogTag, kLutSaId);
    // Publish(this) 把本 SA 登记到 samgr（此后其他模块可 LoadSystemAbility(6901) 拿到代理）
    const bool ok = Publish(this);
    HILOG_INFO(LOG_CORE, "[%{public}s] Publish: %{public}s", kLogTag, ok ? "ok" : "fail");
    if (!ok) {
        return;
    }
    // 预置：创建工作区（引擎在首次 LoadModel 时真正构建 LUT）
    Status st = CreateSession(&g_session);
    HILOG_INFO(LOG_CORE, "[%{public}s] CreateSession: %{public}d", kLogTag, static_cast<int>(st));
}

void LutSystemAbility::OnStop() {
    if (g_session != 0) {
        (void) ReleaseSession(g_session);
        g_session = 0;
    }
    HILOG_INFO(LOG_CORE, "[%{public}s] OnStop, session released", kLogTag);
}

int32_t LutSystemAbility::Dump(int fd, const std::vector<std::u16string> &args) {
    const char *mode = args.empty() ? "summary" : "detail";
    dprintf(fd, "LutSystemAbility (SA_ID=%{public}d) -- LUT-SA 端侧低比特 LLM 推理系统服务\n", kLutSaId);
    dprintf(fd, "  mode        : %s\n", mode);
    dprintf(fd, "  session     : %llu\n", static_cast<unsigned long long>(g_session));
    dprintf(fd, "  kernel      : m128-k3200 LUT (kcfg embedded), 2.44 bit/weight\n");
    dprintf(fd, "  uptime note : OnStart=Publish -> samgr; OnStop=ReleaseSession\n");
    return 0;
}

}  // namespace tmac_sa

#endif  // TMAC_SA_SAMGR_BINDING
