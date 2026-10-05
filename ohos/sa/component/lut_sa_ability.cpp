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
// S6-1：AMS 客户端内检 kit 的**依赖闭包会把 arkcompiler 整个拖进来**（实测：ninja 开始编
// arkcompiler/runtime_core/static_core/...，见 QEMU-DEPLOY.md FIX-64），对 SA 来说体积/耦合都不合适
// → 本版先不直链；换路径（Raw IPC 或应用侧执行）后再用 -DLUTSA_WITH_AMS 打开
#ifdef LUTSA_WITH_AMS
#include "ability_manager_client.h"
#include "want.h"
#else
// S6-1b 路线A：只 include Want（来自 ability_base，依赖很轻）—— 不碰 ability_manager 内检头
#include "want.h"
#include "iservice_registry.h"
#include "if_system_ability_manager.h"
#include "message_parcel.h"
#include "iremote_object.h"
#endif
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
std::string g_lastQuotaNote = "(未判定)";
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

// ---------------------------------------------------------------------------
// S5-2 配额（第一块：可加载模型大小上限）
//   /data/lut_sa/quota.txt（可选，每次 LoadModel 重读）：
//     model_mb=2048      单次可加载模型大小上限（MB；<=0 表示不限制）
//   说明：并发目前**天然串行**（单引擎 + 一把锁，调用方排队），所以"并发配额"暂不需要；
//   会话数配额要等多会话（共享权重、各自 context）落地才有意义 —— 见 PLAN 的 Next。
const char* kQuotaFile = "/data/lut_sa/quota.txt";

long long LoadQuotaModelMb() {
    long long mb = 2048;
    FILE* f = fopen(kQuotaFile, "r");
    if (f == nullptr) {
        return mb;
    }
    char line[96];
    while (fgets(line, sizeof(line), f) != nullptr) {
        long long v = 0;
        if (sscanf(line, "model_mb=%lld", &v) == 1) {
            mb = v;
        }
    }
    fclose(f);
    return mb;
}

// ---------------------------------------------------------------------------
// S6-1 动作白名单：/data/lut_sa/actions_allow.txt（每行一个 bundle；存在且非空 → 只允许表内目标）
//   默认预置 com.ohos.settings（"打开设置"是本环境的端到端靶子，镜像里确实有这个应用）
const char* kActionsAllowFile = "/data/lut_sa/actions_allow.txt";

// ---------------------------------------------------------------------------
// S6-1b 路线A：**自己给 AMS 发 IPC**（不链 ability 内检 kit —— 它的依赖闭包会拖进 arkcompiler，FIX-64）。
// 协议常量与 parcel 顺序**抄自 OH 源码**（换 OH 版本时要回来核对）：
//   SA id       : ABILITY_MGR_SERVICE_ID = 180
//   descriptor  : u"ohos.aafwk.AbilityManager"（ability_manager_interface.h DECLARE_INTERFACE_DESCRIPTOR）
//   code        : AbilityManagerInterfaceCode::START_ABILITY = 1001
//   parcel      : WriteInterfaceToken → WriteParcelable(&want) → WriteInt32(userId) → WriteInt32(requestCode)
//                 → WriteUint64(specifiedFullTokenId)，reply 一个 Int32 返回码
//                 （抄自 services/abilitymgr/src/ability_manager_proxy.cpp 的 StartAbility）
constexpr int32_t kAmsSaId = 180;
constexpr uint32_t kAmsStartAbilityCode = 1001;
const char16_t kAmsDescriptor[] = u"ohos.aafwk.AbilityManager";
std::string g_lastActionNote = "(未执行)";

void LoadActionsAllow(std::vector<std::string>& out, bool* whitelist_mode) {
    out.clear();
    *whitelist_mode = false;
    FILE* f = fopen(kActionsAllowFile, "r");
    if (f == nullptr) {
        out.push_back("com.ohos.settings");   // 默认只放这一个
        return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f) != nullptr) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
            s.pop_back();
        }
        if (!s.empty()) {
            out.push_back(s);
        }
    }
    fclose(f);
    if (!out.empty()) {
        *whitelist_mode = true;
    } else {
        out.push_back("com.ohos.settings");
    }
}

bool ActionTargetAllowed(const std::string& bundle, std::string* why) {
    std::vector<std::string> allow;
    bool wl = false;
    LoadActionsAllow(allow, &wl);
    for (const auto& a : allow) {
        if (a == bundle) {
            return true;
        }
    }
    std::string list;
    for (size_t i = 0; i < allow.size() && i < 8; ++i) {
        list += allow[i];
        list += " ";
    }
    *why = "bundle '" + bundle + "' 不在动作白名单里（当前允许: " + list +
           (wl ? "，来自 " + std::string(kActionsAllowFile) : "，默认值") + "）";
    return false;
}

uint64_t FileSizeBytes(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(st.st_size);
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
    // S5-2 配额：模型大小上限（每次重读配置；超限就带原因拒绝，别把内存吃光）
    {
        const long long mb = LoadQuotaModelMb();
        const uint64_t sz = FileSizeBytes(path.c_str());
        if (mb > 0 && sz > static_cast<uint64_t>(mb) * 1024ULL * 1024ULL) {
            char d[192];
            snprintf(d, sizeof(d), "quota: model %llu MB > limit %lld MB (文件 %s)",
                     static_cast<unsigned long long>(sz / (1024ULL * 1024ULL)), mb, path.c_str());
            g_lastQuotaNote = d;
            HILOG_ERROR(LOG_CORE, "[LutSa] LoadModel denied by quota: %{public}s", d);
            return ERR_INVALID_VALUE;   // 22：调用方按"参数/配额不满足"处理
        }
        g_lastQuotaNote = "quota: ok";
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
             " | last_action: " + g_lastActionNote +
             " | quota: model<=" + std::to_string(LoadQuotaModelMb()) + "MB (" + g_lastQuotaNote +
             ") | " + ::tmac_sa::EngineInfo();
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

ErrCode LutSystemAbility::ExecuteAction(const std::string &action, const std::string &arg,
                                       std::string &result) {
    if (!InferAllowed("ExecuteAction")) {   // S5-1 准入：Tier-A / 白名单
        return kErrPermissionDenied;
    }
    if (action != "start_ability") {
        result = "unsupported action '" + action + "'（本版只支持 start_ability）";
        g_lastActionNote = result;
        return ERR_INVALID_VALUE;
    }

    // arg 语法：<bundle>[/<ability>][@<userId>][#<module>]
    //   （显式启动一般要 ability 名；userId 缺省 -1=默认用户；module 名可选 —— 让取证一轮试多种组合）
    std::string spec = arg;
    std::string module;
    int32_t userId = -1;
    const size_t hash = spec.find('#');
    if (hash != std::string::npos) {
        module = spec.substr(hash + 1);
        spec = spec.substr(0, hash);
    }
    const size_t at = spec.find('@');
    if (at != std::string::npos) {
        userId = static_cast<int32_t>(strtol(spec.c_str() + at + 1, nullptr, 10));
        spec = spec.substr(0, at);
    }
    std::string bundle = spec;
    std::string ability;
    const size_t slash = spec.find('/');
    if (slash != std::string::npos) {
        bundle = spec.substr(0, slash);
        ability = spec.substr(slash + 1);
    }
    if (bundle.empty()) {
        result = "arg 需要 <bundleName>[/<abilityName>]";
        g_lastActionNote = result;
        return ERR_INVALID_VALUE;
    }

    std::string why;
    if (!ActionTargetAllowed(bundle, &why)) {   // S6-1 安全边界
        result = "denied: " + why;
        g_lastActionNote = result;
        HILOG_ERROR(LOG_CORE, "[LutSa] ExecuteAction denied: %{public}s", result.c_str());
        return kErrPermissionDenied;
    }

#ifdef LUTSA_WITH_AMS
    AAFwk::Want want;   // Want 在 OHOS::AAFwk 命名空间下
    want.SetBundle(bundle);
    if (!ability.empty()) {
        want.SetAbilityName(ability);
    }
    const ErrCode ams = AAFwk::AbilityManagerClient::GetInstance()->StartAbility(want);
    char b[320];
    snprintf(b, sizeof(b), "start_ability %s -> AMS ErrCode=%d %s", arg.c_str(),
             static_cast<int>(ams), ams == 0 ? "(已受理)" : "(AMS 拒绝/失败，原因见 AMS 日志)");
    result = b;
    g_lastActionNote = result;
    // 如实回传 AMS 的结果：受理=0；AMS 拒绝就把它的错误码原样带给调用方（不美化）
    return ams == 0 ? ERR_OK : ERR_INVALID_VALUE;
#else
    // 路线A：Raw IPC 直调 AMS（策略已在上面通过：准入 + 动作白名单）
    sptr<ISystemAbilityManager> samgr =
        SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    sptr<IRemoteObject> ams = (samgr != nullptr) ? samgr->GetSystemAbility(kAmsSaId) : nullptr;
    if (ams == nullptr) {
        result = "start_ability " + arg + " -> 拿不到 AMS(SA 180)（samgr 未就绪或 AMS 未启动）";
        g_lastActionNote = result;
        return ERR_INVALID_VALUE;
    }

    AAFwk::Want want;   // Want 在 OHOS::AAFwk 命名空间下
    if (!ability.empty()) {
        want.SetElementName(bundle, ability);   // 这个 OH 版本没有 SetAbilityName；用 SetElementName
    } else {
        want.SetBundle(bundle);                 // 只给 bundle → 让 AMS 自己解析入口 Ability
    }
    if (!module.empty()) {
        want.SetModuleName(module);             // 显式启动时某些路径要求 module 名（S6-1b-2 试出来的）
    }
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt(MessageOption::TF_SYNC);
    if (!data.WriteInterfaceToken(kAmsDescriptor) || !data.WriteParcelable(&want) ||
        !data.WriteInt32(userId) /*-1 = 默认用户，或调用方指定*/ || !data.WriteInt32(0) /*requestCode*/ ||
        !data.WriteUint64(0) /*specifiedFullTokenId*/) {
        result = "start_ability " + arg + " -> parcel 写入失败（协议与 OH 版本不符？见 FIX-66）";
        g_lastActionNote = result;
        return ERR_INVALID_VALUE;
    }
    const int32_t ret = ams->SendRequest(kAmsStartAbilityCode, data, reply, opt);
    if (ret != 0) {
        char b[192];
        snprintf(b, sizeof(b), "start_ability %s -> SendRequest ret=%d（AMS 侧无响应/被拦）", arg.c_str(),
                 static_cast<int>(ret));
        result = b;
        g_lastActionNote = result;
        return ERR_INVALID_VALUE;
    }
    const int32_t amsErr = reply.ReadInt32();
    char b[256];
    snprintf(b, sizeof(b), "start_ability %s -> AMS ErrCode=%d (bundle=%s ability=%s module=%s userId=%d)%s",
             arg.c_str(), static_cast<int>(amsErr), bundle.c_str(), ability.c_str(),
             module.empty() ? "-" : module.c_str(), userId,
             amsErr == 0 ? " 已受理" : " 拒绝/失败");
    result = b;
    g_lastActionNote = result;
    // 如实回传 AMS 的返回码：受理=0；拒绝原样带回（不美化）
    return amsErr == 0 ? ERR_OK : ERR_INVALID_VALUE;
#endif
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
