// lut_screen.cpp —— S7-1b：SA 内的读屏实现（无障碍元素树）
//
// 走的是 S7-1a 实测通过的那条原生口子：AccessibilityUITestAbility
//   RegisterAbilityListener（要进程 native token 持有 ACCESSIBILITY_EXTENSION_ABILITY，
//   本 SA 由 init 按其 cfg 的 permission 字段授予）→ Connect(userId) → OnAbilityConnected → 读树。
// 权限/门链的完整证据见 ohos/sa/evidence/60 与 QEMU-DEPLOY.md FIX-83。
//
// 实测已知的坑（都在这里处理）：
//   · 顶层窗口（桌面/锁屏）深度 4 以内全是容器 —— 所以要下钻到 8 层，并走**所有窗口**；
//   · 文本元素占少数 —— 只回传「带文本或可点」的节点，IPC 载荷才有界；
//   · 连接是进程级单例（4003 = CONNECTION_EXIST 表示已连）→ 单例 + 互斥串行化。
#include "lut_screen.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#include "accessibility_ui_test_ability.h"
#include "accessible_ability_listener.h"
#include "hilog/log.h"

using namespace OHOS;
using namespace OHOS::Accessibility;

namespace tmac_sa {
namespace {

constexpr int kDefaultMaxNodes = 40;
constexpr int kHardMaxNodes = 200;   // 硬上限：再多也不给（保护无障碍服务与 IPC 载荷）
constexpr int kMaxDepth = 8;
constexpr int kMaxKids = 40;
constexpr int kConnectWaitMs = 5000;
constexpr int kMaxTextLen = 160;

std::mutex g_screenMutex;                  // 串行化：IPC 是多线程的
std::atomic<bool> g_connected{false};
std::string g_lastError = "(none)";        // 只记错误，不记屏幕内容

// 最小监听器：只关心 connect/disconnect 两个信号（事件与按键一律忽略 —— 本能力只读）
class ScreenListener : public AccessibleAbilityListener {
public:
    void OnAbilityConnected() override { g_connected.store(true); }
    void OnAbilityDisconnected() override { g_connected.store(false); }
    void OnAccessibilityEvent(const AccessibilityEventInfo &) override {}
    bool OnKeyPressEvent(const std::shared_ptr<MMI::KeyEvent> &) override { return false; }
};

std::shared_ptr<ScreenListener> g_listener;

// JSON 字符串转义：转义 " 与 \，控制字符降级成空格，截断到 kMaxTextLen
std::string Esc(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char ch : in) {
        unsigned char u = static_cast<unsigned char>(ch);
        if (u == '"' || u == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(u));
        } else if (u < 32 || u == 127) {
            out.push_back(' ');
        } else {
            out.push_back(static_cast<char>(u));
        }
    }
    if (out.size() > kMaxTextLen) {
        out.resize(kMaxTextLen);
        out += "...";
    }
    return out;
}

struct Stats {
    int printed = 0;
    int withText = 0;
    int clickable = 0;
    int visited = 0;
};

// 容器节点不返回（对上层没用），只回「带文本或可点」的节点
void AppendNode(std::string &out, const AccessibilityElementInfo &e, int depth, Stats &st, int budget)
{
    ++st.visited;
    const std::string text = e.GetContent();
    const bool clickable = e.IsClickable();
    if (!text.empty()) {
        ++st.withText;
    }
    if (clickable) {
        ++st.clickable;
    }
    if (st.printed >= budget || (text.empty() && !clickable)) {
        return;
    }
    const Rect &box = e.GetRectInScreen();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "%s{\"d\":%d,\"win\":%d,\"a11yId\":%" PRId64 ",\"type\":\"%s\",\"text\":\"%s\","
             "\"click\":%d,\"vis\":%d,\"box\":[%d,%d,%d,%d]}",
             st.printed == 0 ? "" : ",", depth, e.GetWindowId(), e.GetAccessibilityId(),
             Esc(e.GetComponentType()).c_str(), Esc(text).c_str(), clickable ? 1 : 0,
             e.IsVisible() ? 1 : 0, box.GetLeftTopXScreenPostion(), box.GetLeftTopYScreenPostion(),
             box.GetRightBottomXScreenPostion(), box.GetRightBottomYScreenPostion());
    out += buf;
    ++st.printed;
}

// 广度优先 + 硬上限；返回访问到的节点总数
void WalkTree(AccessibilityUITestAbility &ability, const AccessibilityElementInfo &root,
              std::string &nodes, Stats &st, int budget)
{
    AppendNode(nodes, root, 0, st, budget);
    std::vector<AccessibilityElementInfo> level{root};
    for (int depth = 1; depth <= kMaxDepth && st.printed < budget && !level.empty(); ++depth) {
        std::vector<AccessibilityElementInfo> next;
        for (const auto &parent : level) {
            std::vector<AccessibilityElementInfo> kids;
            if (ability.GetChildren(parent, kids) != RET_OK) {
                continue;
            }
            for (const auto &k : kids) {
                if (st.printed >= budget) {
                    break;
                }
                AppendNode(nodes, k, depth, st, budget);
                if (next.size() < kMaxKids) {
                    next.push_back(k);
                }
            }
        }
        level.swap(next);
    }
}

// 首次调用时建立连接（幂等）；返回 true = 已连上
bool EnsureConnected(AccessibilityUITestAbility &ability)
{
    if (g_connected.load()) {
        return true;
    }
    g_listener = std::make_shared<ScreenListener>();
    RetError rr = ability.RegisterAbilityListener(g_listener);
    if (rr != RET_OK) {
        // 1005 = 没权限（进程 native token 缺 ACCESSIBILITY_EXTENSION_ABILITY）
        g_lastError = "register_failed_ret_" + std::to_string(static_cast<int>(rr));
        HILOG_ERROR(LOG_CORE, "[LutSa] ReadScreen: RegisterAbilityListener ret=%{public}d",
                    static_cast<int>(rr));
        return false;
    }
    RetError rc = ability.Connect(0);
    if (rc != RET_OK && rc != RET_ERR_CONNECTION_EXIST) {
        RetError rc2 = ability.Connect(100);   // OH 的默认用户档
        if (rc2 != RET_OK && rc2 != RET_ERR_CONNECTION_EXIST) {
            g_lastError = "connect_failed_ret0_" + std::to_string(static_cast<int>(rc)) +
                          "_ret100_" + std::to_string(static_cast<int>(rc2));
            HILOG_ERROR(LOG_CORE, "[LutSa] ReadScreen: Connect ret0=%{public}d ret100=%{public}d",
                        static_cast<int>(rc), static_cast<int>(rc2));
            return false;
        }
    }
    for (int waited = 0; waited < kConnectWaitMs && !g_connected.load(); waited += 100) {
        usleep(100 * 1000);
    }
    if (!g_connected.load()) {
        g_lastError = "connect_callback_timeout";
        HILOG_ERROR(LOG_CORE, "[LutSa] ReadScreen: wait OnAbilityConnected timeout");
        return false;
    }
    g_lastError = "(none)";
    return true;
}

std::string FailJson(const std::string &why, long elapsedMs)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":0,\"error\":\"%s\",\"connected\":%d,\"elapsed_ms\":%ld}",
             Esc(why).c_str(), g_connected.load() ? 1 : 0, elapsedMs);
    return buf;
}
}  // namespace

std::string ReadScreenJson(int maxNodes)
{
    const auto t0 = std::chrono::steady_clock::now();
    int budget = (maxNodes <= 0) ? kDefaultMaxNodes : maxNodes;
    if (budget > kHardMaxNodes) {
        budget = kHardMaxNodes;
    }

    auto ability = AccessibilityUITestAbility::GetInstance();
    if (ability == nullptr) {
        return FailJson("no_ability_instance", 0);
    }

    std::lock_guard<std::mutex> lock(g_screenMutex);   // 串行化：单例连接 + 有界遍历
    if (!EnsureConnected(*ability)) {
        long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        return FailJson(g_lastError, ms);
    }

    // 窗口列表（id/type/layer）——S7-2 判断"现在在哪一层界面"要用
    std::vector<AccessibilityWindowInfo> windows;
    RetError rw = ability->GetWindows(windows);
    std::string winJson;
    for (size_t i = 0; i < windows.size() && i < 12; ++i) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s{\"id\":%d,\"type\":%d,\"layer\":%d}", i ? "," : "",
                 windows[i].GetWindowId(), static_cast<int>(windows[i].GetWindowType()),
                 windows[i].GetWindowLayer());
        winJson += buf;
    }

    // 树：先活动窗口，再遍历所有窗口（实测：文本常在应用窗口，不在顶层桌面）
    std::string nodes;
    Stats st;
    AccessibilityElementInfo root;
    int32_t rootWindowId = -1;   // 活动窗口已经走过一遍，下面"遍历所有窗口"要跳过它
    if (ability->GetRoot(root) == RET_OK) {
        rootWindowId = root.GetWindowId();
        WalkTree(*ability, root, nodes, st, budget);
    }
    for (auto &w : windows) {
        if (st.printed >= budget) {
            break;
        }
        if (w.GetWindowId() == rootWindowId) {
            continue;   // 实测教训：不跳过的话同一棵子树会被算两遍（nodes 里出现重复 a11yId）
        }
        AccessibilityElementInfo wr;
        if (ability->GetRootByWindow(w, wr) == RET_OK) {
            WalkTree(*ability, wr, nodes, st, budget);
        }
    }

    long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    char head[256];
    snprintf(head, sizeof(head),
             "{\"ok\":1,\"connected\":%d,\"user\":%d,\"elapsed_ms\":%ld,\"getwin_ret\":%d,"
             "\"budget\":%d,\"truncated\":%d,",
             g_connected.load() ? 1 : 0, ability->GetCurrentUserId(), ms, static_cast<int>(rw),
             budget, (st.printed >= budget) ? 1 : 0);
    std::string out = head;
    out += "\"windows\":[" + winJson + "],";
    char counts[160];
    snprintf(counts, sizeof(counts),
             "\"counts\":{\"nodes\":%d,\"withText\":%d,\"clickable\":%d},", st.printed, st.withText,
             st.clickable);
    out += counts;
    out += "\"nodes\":[" + nodes + "]}";

    // 隐私：日志只记计数与耗时，**不记屏幕原文**
    HILOG_INFO(LOG_CORE, "[LutSa] ReadScreen ok: nodes=%{public}d withText=%{public}d clickable=%{public}d ms=%{public}ld",
               st.printed, st.withText, st.clickable, ms);
    return out;
}

void ReleaseScreen()
{
    std::lock_guard<std::mutex> lock(g_screenMutex);
    auto ability = AccessibilityUITestAbility::GetInstance();
    if (ability != nullptr && g_connected.load()) {
        ability->Disconnect(0);
        HILOG_INFO(LOG_CORE, "[LutSa] ReadScreen: disconnected");
    }
    g_connected.store(false);
    g_listener = nullptr;
}

// ── S7-2-A：结构化快照（与 JSON 版共用连接/锁/有界遍历，只是换个输出形态）────
namespace {
void AppendSnapElement(ScreenSnapshot &s, const AccessibilityElementInfo &e)
{
    const std::string text = e.GetContent();
    const bool clickable = e.IsClickable();
    if (text.empty() && !clickable) {
        return;   // 容器节点不进 elements（与 JSON 版一致）
    }
    const Rect &box = e.GetRectInScreen();
    ScreenElement el;
    el.a11yId = e.GetAccessibilityId();
    el.winId = e.GetWindowId();
    el.type = e.GetComponentType();
    el.text = text;
    el.clickable = clickable;
    el.visible = e.IsVisible();
    el.x1 = box.GetLeftTopXScreenPostion();
    el.y1 = box.GetLeftTopYScreenPostion();
    el.x2 = box.GetRightBottomXScreenPostion();
    el.y2 = box.GetRightBottomYScreenPostion();
    s.elements.push_back(el);
}

void WalkSnapLevel(AccessibilityUITestAbility &ability,
                   const std::vector<AccessibilityElementInfo> &level, int depth, ScreenSnapshot &s,
                   int budget)
{
    std::vector<AccessibilityElementInfo> next;
    for (const auto &e : level) {
        if (s.visited >= budget) {
            s.truncated = true;
            return;
        }
        ++s.visited;
        const std::string text = e.GetContent();
        if (!text.empty()) {
            ++s.withText;
        }
        if (e.IsClickable()) {
            ++s.clickable;
        }
        AppendSnapElement(s, e);
        std::vector<AccessibilityElementInfo> kids;
        if (ability.GetChildren(e, kids) != RET_OK) {
            continue;
        }
        int kept = 0;
        for (const auto &k : kids) {
            if (kept++ >= kMaxKids) {
                break;
            }
            next.push_back(k);
        }
    }
    if (!next.empty()) {
        WalkSnapLevel(ability, next, depth + 1, s, budget);
    }
}

// 广度优先（与 JSON 版一致）：小预算时也能覆盖整屏宽度，而不是一头扎进第一个子树
void WalkSnap(AccessibilityUITestAbility &ability, const AccessibilityElementInfo &root,
              ScreenSnapshot &s, int budget)
{
    std::vector<AccessibilityElementInfo> level{root};
    WalkSnapLevel(ability, level, 0, s, budget);
}

}  // namespace

ScreenSnapshot ReadScreenSnapshot(int maxNodes)
{
    ScreenSnapshot s;
    const auto t0 = std::chrono::steady_clock::now();
    int budget = (maxNodes <= 0) ? kDefaultMaxNodes : maxNodes;
    if (budget > kHardMaxNodes) {
        budget = kHardMaxNodes;
    }
    auto ability = AccessibilityUITestAbility::GetInstance();
    if (ability == nullptr) {
        s.error = "no_ability_instance";
        return s;
    }
    std::lock_guard<std::mutex> lock(g_screenMutex);   // 与 JSON 版共用同一把锁
    if (!EnsureConnected(*ability)) {
        s.error = g_lastError;
        s.connected = g_connected.load() ? 1 : 0;
        return s;
    }
    s.connected = 1;
    s.user = ability->GetCurrentUserId();
    std::vector<AccessibilityWindowInfo> windows;
    if (ability->GetWindows(windows) == RET_OK) {
        for (size_t i = 0; i < windows.size() && i < 12; ++i) {
            ScreenWindow w;
            w.id = windows[i].GetWindowId();
            w.type = static_cast<int32_t>(windows[i].GetWindowType());
            w.layer = windows[i].GetWindowLayer();
            s.windows.push_back(w);
        }
    }
    AccessibilityElementInfo root;
    int32_t rootWin = -1;
    if (ability->GetRoot(root) == RET_OK) {
        rootWin = root.GetWindowId();
        WalkSnap(*ability, root, s, budget);
    }
    for (auto &w : windows) {
        if (s.visited >= budget) {
            s.truncated = true;
            break;
        }
        if (w.GetWindowId() == rootWin) {
            continue;   // 活动窗口已走过，跳过避免重复（FIX-88）
        }
        AccessibilityElementInfo wr;
        if (ability->GetRootByWindow(w, wr) == RET_OK) {
            WalkSnap(*ability, wr, s, budget);
        }
    }
    s.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
    s.ok = true;
    // 隐私：日志只记计数（不记屏幕原文）
    HILOG_INFO(LOG_CORE,
               "[LutSa] ReadScreenSnapshot: visited=%{public}d withText=%{public}d clickable=%{public}d ms=%{public}ld",
               s.visited, s.withText, s.clickable, s.elapsedMs);
    return s;
}

std::string ScreenState()
{
    if (g_connected.load()) {
        return "connected";
    }
    return "absent(" + g_lastError + ")";
}
}  // namespace tmac_sa
